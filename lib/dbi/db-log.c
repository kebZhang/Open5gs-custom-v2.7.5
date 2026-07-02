/*
 * db-log.c -- asynchronous NF<->MongoDB access logging (TYcustom instrumentation)
 *
 * See db-log.h for the design. It is a direct port of lib/sbi/http-log.c:
 *   - ogs_db_log_emit() runs on the NF's event-loop thread. It only formats one
 *     line and hands it to a fixed-size ring; it NEVER does file I/O. The
 *     producer<->writer hand-off holds a mutex only long enough to move an index.
 *   - A single background writer thread drains the ring and appends to the file
 *     with a buffered FILE*. Single writer => lines never interleave.
 *   - Ring full => record dropped and counted; the producer never waits.
 *
 * Timestamp precision: open5gs ogs_time_now() is GMT epoch *microseconds* (not
 * ns). We emit "YYYY-MM-DDTHH:MM:SS.ffffffZ" (UTC, 6 fractional digits), same as
 * http-log.c, so DB and HTTP lines sort together.
 */

#include <stdlib.h> /* calloc/free for the large ring buffer */

#include "ogs-dbi.h"
#include "db-log.h"

#define DB_LOG_DEFAULT_PATH     "/tmp/DB_log.txt"

/* Ring capacity (number of pre-formatted lines). DB ops per registration are far
 * fewer than HTTP messages, so this only fills on extreme bursts. Sized like the
 * HTTP ring: 1<<18 slots * 516 B ~= 129 MB per NF (only UDR and PCF hold one). */
#define DB_LOG_RING_SLOTS       (1 << 18)
/* Worst-case line (long collection + subresource + imsi ueid) is well under this;
 * ogs_snprintf() returns the would-be length, so an over-long line is dropped
 * (not written truncated) to avoid corrupting the JSON stream. */
#define DB_LOG_LINE_MAX         512

#define DB_LOG_FLUSH_INTERVAL   ogs_time_from_msec(200)

/* Size of the stdio full-buffer for the writer's FILE* (see setvbuf in init).
 * Large enough that whole records are batched between explicit flushes. */
#define DB_LOG_STREAM_BUF       (1 * 1024 * 1024) /* 1 MiB (DB volume << HTTP) */

typedef struct db_log_slot_s {
    int len;
    char line[DB_LOG_LINE_MAX];
} db_log_slot_t;

static struct {
    bool inited;

    char nf[16];                 /* this NF's name, e.g. "UDR" */

    db_log_slot_t *ring;         /* DB_LOG_RING_SLOTS entries */
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
    /* len >= MAX means the line was truncated and lacks its closing "}\n".
     * Drop it rather than write a broken JSON line. */
    if (len >= DB_LOG_LINE_MAX) {
        self.dropped++;
        return;
    }

    ogs_thread_mutex_lock(&self.mutex);

    if (self.count >= DB_LOG_RING_SLOTS) {
        /* full -> drop, never block the event loop */
        self.dropped++;
        ogs_thread_mutex_unlock(&self.mutex);
        return;
    }

    memcpy(self.ring[self.tail].line, line, len);
    self.ring[self.tail].len = len;
    self.tail = (self.tail + 1) & (DB_LOG_RING_SLOTS - 1);
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
        db_log_slot_t slot;
        bool have = false;
        bool stopping;

        ogs_thread_mutex_lock(&self.mutex);
        while (self.count == 0 && !self.stop)
            ogs_thread_cond_timedwait(
                    &self.cond, &self.mutex, DB_LOG_FLUSH_INTERVAL);

        if (self.count > 0) {
            slot = self.ring[self.head];
            self.head = (self.head + 1) & (DB_LOG_RING_SLOTS - 1);
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
             * appended right after the truncated prefix, producing a torn line.
             * Loop until the full slot.len bytes are written. This runs only on
             * the background writer thread, so it never adds latency to the NF
             * event loop / UE registration. */
            size_t off = 0;
            while (off < (size_t)slot.len) {
                size_t n = fwrite(slot.line + off, 1,
                        (size_t)slot.len - off, self.fp);
                if (n == 0) {
                    /* Hard write error (not a short write): drop the whole line
                     * (keeps the JSON-Lines stream valid) and count it. */
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
            if (now - last_flush >= DB_LOG_FLUSH_INTERVAL) {
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

/* Insert a per-pod suffix before the file extension so that many NF instances
 * (e.g. 10 UDR pods) each write a distinct file under their own /tmp. The pod
 * name is taken from HOSTNAME, which Kubernetes sets to the pod name by default.
 * If HOSTNAME is unset the base path is used verbatim. Returns a new alloc. */
static char *db_log_path_with_pod(const char *base)
{
    const char *pod = getenv("HOSTNAME");
    const char *dot;

    if (!pod || pod[0] == '\0')
        return ogs_strdup(base);

    dot = strrchr(base, '.');
    if (dot && dot != base)
        return ogs_msprintf("%.*s_%s%s",
                (int)(dot - base), base, pod, dot);
    return ogs_msprintf("%s_%s", base, pod);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void ogs_db_log_init(const char *nf_name)
{
    const char *env;

    if (self.inited)
        return;

    memset(&self, 0, sizeof(self));

    if (nf_name)
        ogs_cpystrn(self.nf, nf_name, sizeof(self.nf));

    env = getenv("DB_LOG_PATH");
    self.path = db_log_path_with_pod(env ? env : DB_LOG_DEFAULT_PATH);
    ogs_assert(self.path);

    self.fp = fopen(self.path, "a");
    if (!self.fp) {
        ogs_error("ogs_db_log_init: cannot open [%s]", self.path);
        ogs_free(self.path);
        self.path = NULL;
        return;
    }

    /* Give the stream a large fully-buffered block. We flush explicitly every
     * DB_LOG_FLUSH_INTERVAL on the writer thread, so a big block reduces syscalls
     * and makes it far less likely that a record's bytes straddle a buffer
     * boundary and get split across two underlying write()s (a torn-line source).
     * This buffer lives on the writer thread's FILE* only; the NF event loop
     * never touches it. */
    if (self.log_buf)
        free(self.log_buf);
    self.log_buf = malloc(DB_LOG_STREAM_BUF);
    if (self.log_buf)
        setvbuf(self.fp, self.log_buf, _IOFBF, DB_LOG_STREAM_BUF);

    /* Long-lived ~0.5 GiB block allocated once. Use libc calloc(), not
     * ogs_calloc(): open5gs' talloc pool rejects a block this large, which would
     * leave the logger un-inited (empty DB_log.txt). malloc has no such cap. */
    self.ring = calloc(DB_LOG_RING_SLOTS, sizeof(db_log_slot_t));
    if (!self.ring) {
        ogs_error("ogs_db_log_init: ring alloc failed");
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

    ogs_info("ogs_db_log_init: nf[%s] writing [%s]", self.nf, self.path);
}

void ogs_db_log_final(void)
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
        ogs_warn("ogs_db_log_final: dropped %llu records "
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

uint64_t ogs_db_log_dropped(void)
{
    return self.dropped + self.write_errors;
}

void ogs_db_log_emit(const char *resource, const char *subresource,
        const char *operation, const char *ueid,
        ogs_time_t req_time, ogs_time_t resp_time)
{
    char req_ts[40];
    char resp_ts[40];
    char line[DB_LOG_LINE_MAX];
    int len;

    if (!self.inited)
        return;

    format_ts(req_time, req_ts, sizeof(req_ts));
    format_ts(resp_time, resp_ts, sizeof(resp_ts));

    len = ogs_snprintf(line, sizeof(line),
            "{\"nf\":\"%s\",\"mongo\":\"mongodb\",\"resource\":\"%s\","
            "\"subresource\":\"%s\",\"operation\":\"%s\",\"ueid\":\"%s\","
            "\"req_time\":\"%s\",\"resp_time\":\"%s\",\"latency_us\":%lld}\n",
            self.nf,
            resource ? resource : "",
            subresource ? subresource : "",
            operation ? operation : "",
            ueid ? ueid : "",
            req_ts, resp_ts,
            (long long)(resp_time - req_time));

    ring_push(line, len);
}
