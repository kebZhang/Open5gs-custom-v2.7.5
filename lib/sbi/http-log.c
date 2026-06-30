/*
 * http-log.c -- asynchronous SBI HTTP access logging (TYcustom instrumentation)
 *
 * See http-log.h for the design. Summary:
 *   - The four emit functions run on the NF's single event-loop thread. They
 *     only capture a timestamp, format one line into a slot, and hand the slot
 *     to a fixed-size ring. They NEVER do file I/O. The producer<->writer
 *     hand-off uses a mutex held only long enough to move an index (no I/O
 *     under the lock), so the event loop is never blocked on disk.
 *   - A single background writer thread drains the ring and appends to the
 *     file with a buffered FILE*. Single writer => lines never interleave.
 *   - Ring full => the record is dropped and counted; the producer never waits.
 *
 * Timestamp precision: open5gs ogs_time_now() is GMT epoch *microseconds*
 * (not ns). We emit "YYYY-MM-DDTHH:MM:SS.ffffffZ" (UTC, 6 fractional digits).
 */

#include <stdlib.h> /* calloc/free for the large ring buffer */

#include "ogs-sbi.h"
#include "http-log.h"

#define HTTP_LOG_DEFAULT_PATH   "/tmp/HTTP_log.txt"

/* Ring capacity (number of pre-formatted lines). Sized to absorb bursts within
 * a flush interval at high load while keeping per-NF memory bounded: at
 * 1<<18 slots * 1028 B ~= 257 MB per NF. With ~11 SBI NFs each holding one ring
 * that is ~2.8 GB cluster-wide (vs ~11 GB at 1<<20). 262144 buffered lines is
 * tens of x headroom for 1000 UE/s with a 200 ms writer flush. */
#define HTTP_LOG_RING_SLOTS     (1 << 18)
/* Worst-case registration line (RSP_RX with ctx_id, suci ueid, longest UDR
 * nudr-dr URI ~160 B) is ~330 B; 1024 gives ~3x headroom. ogs_snprintf()
 * returns the length that *would* have been written, so a line exceeding this
 * is dropped (not written truncated) to avoid corrupting the JSON stream. */
#define HTTP_LOG_LINE_MAX       1024

#define HTTP_LOG_FLUSH_INTERVAL ogs_time_from_msec(200)

/* Size of the stdio full-buffer for the writer's FILE* (see setvbuf in init).
 * Large enough that whole records are batched between explicit flushes. */
#define HTTP_LOG_STREAM_BUF     (4 * 1024 * 1024) /* 4 MiB */

typedef struct http_log_slot_s {
    int len;
    char line[HTTP_LOG_LINE_MAX];
} http_log_slot_t;

static struct {
    bool inited;

    http_log_slot_t *ring;       /* HTTP_LOG_RING_SLOTS entries */
    unsigned int head;           /* writer reads here  (consumer) */
    unsigned int tail;           /* producer writes here          */
    unsigned int count;          /* occupied slots                */

    ogs_thread_mutex_t mutex;
    ogs_thread_cond_t cond;      /* writer waits on this          */
    ogs_thread_t *thread;
    bool stop;

    FILE *fp;
    char *log_buf;               /* stdio full-buffer backing self.fp */
    char *path;

    uint64_t dropped;       /* producer side: ring full / oversize (under mutex) */
    uint64_t write_errors;  /* writer side: hard write errors (writer thread only) */
} self;

/* ------------------------------------------------------------------ */
/* helpers (run on the event-loop thread; all in-memory, no I/O)       */
/* ------------------------------------------------------------------ */

static const char *self_nf_name(void)
{
    if (ogs_sbi_self()->nf_instance &&
            ogs_sbi_self()->nf_instance->nf_type)
        return OpenAPI_nf_type_ToString(
                ogs_sbi_self()->nf_instance->nf_type);
    return "";
}

/*
 * Derive the destination NF name from the SBI service prefix in a URI/path.
 * e.g. ".../nausf-auth/v1/..." -> "AUSF". Returns "" if not recognized.
 * Mirrors the free5gc nfFromServicePrefix() mapping.
 */
static const char *dst_nf_from_uri(const char *uri)
{
    const char *p;
    char seg[16];
    int i;

    if (!uri)
        return "";

    /* find "/n" that starts the service prefix */
    p = strstr(uri, "/n");
    if (!p)
        return "";
    p += 2; /* skip "/n" -> now at "ausf-auth/..." */

    /* copy up to '-' into seg */
    for (i = 0; i < (int)sizeof(seg) - 1 && p[i] && p[i] != '-' &&
            p[i] != '/'; i++)
        seg[i] = p[i];
    seg[i] = 0;
    if (p[i] != '-')
        return "";

    if (!strcmp(seg, "amf")) return "AMF";
    if (!strcmp(seg, "ausf")) return "AUSF";
    if (!strcmp(seg, "udm")) return "UDM";
    if (!strcmp(seg, "udr")) return "UDR";
    if (!strcmp(seg, "nrf")) return "NRF";
    if (!strcmp(seg, "pcf")) return "PCF";
    if (!strcmp(seg, "nssf")) return "NSSF";
    if (!strcmp(seg, "smf")) return "SMF";
    if (!strcmp(seg, "nef")) return "NEF";
    if (!strcmp(seg, "chf")) return "CHF";
    if (!strcmp(seg, "bsf")) return "BSF";
    if (!strcmp(seg, "udsf")) return "UDSF";
    if (!strcmp(seg, "scp")) return "SCP";
    if (!strcmp(seg, "sepp")) return "SEPP";
    return "";
}

/* "YYYY-MM-DDTHH:MM:SS.ffffffZ" from an ogs_time_t (GMT epoch usec). */
static void format_ts(ogs_time_t t, char *buf, size_t sz)
{
    struct tm tm;
    char date[32];

    ogs_gmtime(ogs_time_sec(t), &tm);
    ogs_strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
    ogs_snprintf(buf, sz, "%s.%06dZ", date, (int)ogs_time_usec(t));
}

/* Push one already-formatted line into the ring. Producer side.
 * Holds the mutex only to copy into a slot + bump the index (no I/O). */
static void ring_push(const char *line, int len)
{
    if (!self.inited || !self.ring)
        return;

    if (len <= 0)
        return;
    /* ogs_snprintf() returns the would-be length; len >= MAX means the line was
     * truncated and lacks its closing "}\n". Drop it rather than write a broken
     * JSON line that would corrupt the following record. */
    if (len >= HTTP_LOG_LINE_MAX) {
        self.dropped++;
        return;
    }

    ogs_thread_mutex_lock(&self.mutex);

    if (self.count >= HTTP_LOG_RING_SLOTS) {
        /* full -> drop, never block the event loop */
        self.dropped++;
        ogs_thread_mutex_unlock(&self.mutex);
        return;
    }

    memcpy(self.ring[self.tail].line, line, len);
    self.ring[self.tail].len = len;
    self.tail = (self.tail + 1) & (HTTP_LOG_RING_SLOTS - 1);
    self.count++;

    ogs_thread_mutex_unlock(&self.mutex);
    ogs_thread_cond_signal(&self.cond);
}

/* ------------------------------------------------------------------ */
/* writer thread (the only thread doing file I/O)                      */
/* ------------------------------------------------------------------ */

static void writer_main(void *data)
{
    ogs_time_t last_flush = ogs_time_now();

    for (;;) {
        http_log_slot_t slot;
        bool have = false;
        bool stopping;

        ogs_thread_mutex_lock(&self.mutex);
        while (self.count == 0 && !self.stop)
            ogs_thread_cond_timedwait(
                    &self.cond, &self.mutex, HTTP_LOG_FLUSH_INTERVAL);

        if (self.count > 0) {
            slot = self.ring[self.head];
            self.head = (self.head + 1) & (HTTP_LOG_RING_SLOTS - 1);
            self.count--;
            have = true;
        }
        stopping = self.stop && self.count == 0;
        ogs_thread_mutex_unlock(&self.mutex);

        if (have && self.fp) {
            /* Write the whole line or drop it -- never leave a partial line in
             * the file. fwrite() can return a short count if interrupted by a
             * signal or on a partial underlying write; without this loop the
             * tail of the record is silently lost and the next record is
             * appended right after the truncated prefix, producing a torn line
             * (e.g. {"event":"R{"event":...). Loop until the full slot.len bytes
             * are written. This runs only on the background writer thread, so it
             * never adds latency to the NF event loop / UE registration. */
            size_t off = 0;
            while (off < (size_t)slot.len) {
                size_t n = fwrite(slot.line + off, 1,
                        (size_t)slot.len - off, self.fp);
                if (n == 0) {
                    /* Hard write error (not a short write): give up on this
                     * line, count it, and resync to the next record boundary.
                     * Dropping a whole line keeps the JSON-Lines stream valid
                     * (offline parser skips one line) rather than corrupting it
                     * with a partial record. */
                    if (ferror(self.fp)) {
                        clearerr(self.fp);
                        /* writer-thread-only counter: no mutex needed, never
                         * touched by the producer hot path */
                        self.write_errors++;
                    }
                    break;
                }
                off += n;
            }
        }

        if (self.fp) {
            ogs_time_t now = ogs_time_now();
            if (now - last_flush >= HTTP_LOG_FLUSH_INTERVAL) {
                fflush(self.fp);
                last_flush = now;
            }
        }

        if (stopping)
            break;
    }

    if (self.fp)
        fflush(self.fp);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void ogs_http_log_init(void)
{
    const char *env;

    if (self.inited)
        return;

    memset(&self, 0, sizeof(self));

    env = getenv("HTTP_LOG_PATH");
    self.path = ogs_strdup(env ? env : HTTP_LOG_DEFAULT_PATH);
    ogs_assert(self.path);

    self.fp = fopen(self.path, "a");
    if (!self.fp) {
        ogs_error("ogs_http_log_init: cannot open [%s]", self.path);
        ogs_free(self.path);
        self.path = NULL;
        return;
    }

    /* Give the stream a large fully-buffered block. We flush explicitly every
     * HTTP_LOG_FLUSH_INTERVAL on the writer thread, so line/no buffering would
     * only add syscalls. A big block also makes it far less likely that a single
     * record's bytes straddle a buffer boundary and get split across two
     * underlying write()s (a source of torn lines). This buffer lives on the
     * writer thread's FILE* only and never touches the NF event loop. */
    if (self.log_buf)
        free(self.log_buf);
    self.log_buf = malloc(HTTP_LOG_STREAM_BUF);
    if (self.log_buf)
        setvbuf(self.fp, self.log_buf, _IOFBF, HTTP_LOG_STREAM_BUF);

    /* The ring is a single ~1 GiB long-lived block allocated once at startup.
     * Use the libc allocator, not ogs_calloc(): open5gs' talloc pool rejects a
     * block this large (it failed here with "ring alloc failed"), which left the
     * logger un-inited so nothing was ever written. malloc has no such cap. */
    self.ring = calloc(HTTP_LOG_RING_SLOTS, sizeof(http_log_slot_t));
    if (!self.ring) {
        ogs_error("ogs_http_log_init: ring alloc failed");
        fclose(self.fp);
        self.fp = NULL;
        ogs_free(self.path);
        self.path = NULL;
        return;
    }

    ogs_thread_mutex_init(&self.mutex);
    ogs_thread_cond_init(&self.cond);

    self.head = self.tail = self.count = 0;
    self.stop = false;
    self.dropped = 0;
    self.inited = true;

    self.thread = ogs_thread_create(writer_main, NULL);
    ogs_assert(self.thread);

    ogs_info("ogs_http_log_init: writing [%s]", self.path);
}

void ogs_http_log_final(void)
{
    if (!self.inited)
        return;

    ogs_thread_mutex_lock(&self.mutex);
    self.stop = true;
    ogs_thread_mutex_unlock(&self.mutex);
    ogs_thread_cond_signal(&self.cond);

    if (self.thread)
        ogs_thread_destroy(self.thread);

    if (self.fp) {
        fflush(self.fp);
        fclose(self.fp); /* flushes & detaches the setvbuf buffer */
        self.fp = NULL;
    }

    if (self.log_buf) {
        free(self.log_buf); /* safe: freed only after fclose detached it */
        self.log_buf = NULL;
    }

    if (self.ring) {
        free(self.ring); /* paired with calloc() in init */
        self.ring = NULL;
    }

    ogs_thread_cond_destroy(&self.cond);
    ogs_thread_mutex_destroy(&self.mutex);

    if (self.dropped || self.write_errors)
        ogs_warn("ogs_http_log_final: dropped %llu records "
                "(ring-full %llu, write-error %llu)",
                (unsigned long long)(self.dropped + self.write_errors),
                (unsigned long long)self.dropped,
                (unsigned long long)self.write_errors);

    if (self.path) {
        ogs_free(self.path);
        self.path = NULL;
    }

    self.inited = false;
}

uint64_t ogs_http_log_dropped(void)
{
    return self.dropped + self.write_errors;
}

void ogs_http_log_request(const char *event,
        ogs_sbi_request_t *request, const char *ue_id)
{
    ogs_time_t now;
    char ts[40];
    char line[HTTP_LOG_LINE_MAX];
    const char *src, *dst;
    int len;

    if (!self.inited || !request)
        return;

    now = ogs_time_now();
    format_ts(now, ts, sizeof(ts));

    /* REQ_TX: src=self, dst=from uri ; REQ_RX: src=User-Agent, dst=self */
    if (!strcmp(event, OGS_HTTP_LOG_REQ_RX)) {
        const char *ua = NULL;
        if (request->http.headers)
            ua = ogs_sbi_header_get(request->http.headers, OGS_SBI_USER_AGENT);
        src = ua ? ua : "";
        dst = self_nf_name();
    } else { /* REQ_TX */
        src = self_nf_name();
        dst = dst_nf_from_uri(request->h.uri);
    }

    len = ogs_snprintf(line, sizeof(line),
            "{\"event\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\","
            "\"method\":\"%s\",\"uri\":\"%s\",\"ueid\":\"%s\",\"ts\":\"%s\"}\n",
            event, src, dst,
            request->h.method ? request->h.method : "",
            request->h.uri ? request->h.uri : "",
            ue_id ? ue_id : "",
            ts);

    ring_push(line, len);
}

void ogs_http_log_response(const char *event,
        ogs_sbi_response_t *response, const char *ue_id, const char *ctx_id)
{
    ogs_time_t now;
    char ts[40];
    char line[HTTP_LOG_LINE_MAX];
    const char *src, *dst;
    int len;

    if (!self.inited || !response)
        return;

    now = ogs_time_now();
    format_ts(now, ts, sizeof(ts));

    /* RSP_RX: this NF received a response (src=peer from uri, dst=self).
     * RSP_TX: this NF is sending a response (src=self, dst=peer from uri). */
    if (!strcmp(event, OGS_HTTP_LOG_RSP_RX)) {
        src = dst_nf_from_uri(response->h.uri);
        dst = self_nf_name();
    } else { /* RSP_TX */
        src = self_nf_name();
        dst = dst_nf_from_uri(response->h.uri);
    }

    if (ctx_id && *ctx_id) {
        len = ogs_snprintf(line, sizeof(line),
                "{\"event\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\","
                "\"method\":\"%s\",\"uri\":\"%s\",\"ueid\":\"%s\","
                "\"ctx_id\":\"%s\",\"ts\":\"%s\"}\n",
                event, src, dst,
                response->h.method ? response->h.method : "",
                response->h.uri ? response->h.uri : "",
                ue_id ? ue_id : "",
                ctx_id, ts);
    } else {
        len = ogs_snprintf(line, sizeof(line),
                "{\"event\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\","
                "\"method\":\"%s\",\"uri\":\"%s\",\"ueid\":\"%s\",\"ts\":\"%s\"}\n",
                event, src, dst,
                response->h.method ? response->h.method : "",
                response->h.uri ? response->h.uri : "",
                ue_id ? ue_id : "",
                ts);
    }

    ring_push(line, len);
}

void ogs_http_log_rsp_tx(
        ogs_sbi_response_t *response, ogs_sbi_request_t *req, int wq)
{
    ogs_time_t now;
    char ts[40];
    char line[HTTP_LOG_LINE_MAX];
    const char *method, *uri, *dst;
    int len;

    if (!self.inited || !response)
        return;

    now = ogs_time_now();
    format_ts(now, ts, sizeof(ts));

    /* The response has no h.uri/h.method; take them from the originating
     * request. src is self. dst is the peer this response goes back to: it is
     * the NF that SENT the request, which open5gs records in the request's
     * User-Agent header (same source REQ_RX uses for its src). The request
     * (and its headers) is still alive here -- stream->request is freed only at
     * stream teardown, after send_response. ueid is left empty (attributed
     * offline by pairing on the request's (method, uri-path)). */
    method = (req && req->h.method) ? req->h.method : "";
    uri = (req && req->h.uri) ? req->h.uri : "";
    dst = "";
    if (req && req->http.headers) {
        const char *ua =
                ogs_sbi_header_get(req->http.headers, OGS_SBI_USER_AGENT);
        if (ua)
            dst = ua;
    }

    /* wq = session userspace write_queue depth after this response completed
     * HTTP/2 serialization. It includes this response's own pkbuf(s), plus any
     * older queued output. The real socket write occurs later via OGS_POLLOUT.
     */
    len = ogs_snprintf(line, sizeof(line),
            "{\"event\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\","
            "\"method\":\"%s\",\"uri\":\"%s\",\"ueid\":\"%s\","
            "\"wq\":%d,\"ts\":\"%s\"}\n",
            OGS_HTTP_LOG_RSP_TX, self_nf_name(), dst,
            method, uri, "", wq, ts);

    ring_push(line, len);
}
