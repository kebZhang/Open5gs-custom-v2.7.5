/*
 * udr-lat-log.c -- asynchronous per-request UDR latency logging
 *                  (TYcustom instrumentation)
 *
 * See udr-lat-log.h for the design. The emit function runs on the UDR
 * event-loop thread: it only reads the four timestamps already stored on the
 * request, formats one line, and memcpy's it into a fixed-size ring. A single
 * background writer thread drains the ring and appends to the file with a
 * buffered FILE*. Ring full => drop & count, never block. Single writer =>
 * lines never interleave. Mechanism mirrors lib/sbi/http-log.c.
 *
 * Timestamp precision: ogs_time_now() is GMT epoch *microseconds*. Each of the
 * five times is emitted as "YYYY-MM-DDTHH:MM:SS.ffffffZ" (UTC, 6 frac digits);
 * an unset time (0) is emitted as "".
 */

#include <stdlib.h> /* calloc/free for the large ring buffer */

#include "ogs-sbi.h"
#include "udr-lat-log.h"

#define UDR_LAT_LOG_DEFAULT_PATH    "/tmp/UDR_log.txt"

/*
 * Ring capacity (number of pre-formatted lines). One slot ~1 KiB, so 1<<18 =
 * 262144 slots ~= 256 MB. The host has plenty of RAM (per requirement), and
 * this is tens of x headroom for 1000 req/s with a 200 ms writer flush.
 */
#define UDR_LAT_LOG_RING_SLOTS      (1 << 18)
/* One line carries 4 fixed fields + 5 timestamps (~28 B each) + a long
 * nudr-dr URI (~160 B). 1024 B leaves ample headroom for current UDR paths.
 * ogs_snprintf() returns the would-be length, so an oversize line is dropped
 * (not written truncated) to keep the JSON-Lines stream valid. */
#define UDR_LAT_LOG_LINE_MAX        1024

#define UDR_LAT_LOG_FLUSH_INTERVAL  ogs_time_from_msec(200)

/* stdio full-buffer for the writer's FILE* (batches records between flushes). */
#define UDR_LAT_LOG_STREAM_BUF      (4 * 1024 * 1024) /* 4 MiB */

typedef struct udr_lat_log_slot_s {
    int len;
    char line[UDR_LAT_LOG_LINE_MAX];
} udr_lat_log_slot_t;

static struct {
    bool inited;

    udr_lat_log_slot_t *ring;    /* UDR_LAT_LOG_RING_SLOTS entries */
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

/* "YYYY-MM-DDTHH:MM:SS.ffffffZ" from an ogs_time_t (GMT epoch usec).
 * An unset time (0) yields an empty string. */
static void format_ts(ogs_time_t t, char *buf, size_t sz)
{
    struct tm tm;
    char date[32];

    if (t == 0) {
        if (sz)
            buf[0] = 0;
        return;
    }

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
    if (len >= UDR_LAT_LOG_LINE_MAX) {
        self.dropped++;
        return;
    }

    ogs_thread_mutex_lock(&self.mutex);

    if (self.count >= UDR_LAT_LOG_RING_SLOTS) {
        /* full -> drop, never block the event loop */
        self.dropped++;
        ogs_thread_mutex_unlock(&self.mutex);
        return;
    }

    memcpy(self.ring[self.tail].line, line, len);
    self.ring[self.tail].len = len;
    self.tail = (self.tail + 1) & (UDR_LAT_LOG_RING_SLOTS - 1);
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
        udr_lat_log_slot_t slot;
        bool have = false;
        bool stopping;

        ogs_thread_mutex_lock(&self.mutex);
        while (self.count == 0 && !self.stop)
            ogs_thread_cond_timedwait(
                    &self.cond, &self.mutex, UDR_LAT_LOG_FLUSH_INTERVAL);

        if (self.count > 0) {
            slot = self.ring[self.head];
            self.head = (self.head + 1) & (UDR_LAT_LOG_RING_SLOTS - 1);
            self.count--;
            have = true;
        }
        stopping = self.stop && self.count == 0;
        ogs_thread_mutex_unlock(&self.mutex);

        if (have && self.fp) {
            /* Write the whole line or drop it -- never leave a partial line in
             * the file (fwrite can return short on signal/partial write). This
             * runs only on the writer thread, never on the UDR event loop. */
            size_t off = 0;
            while (off < (size_t)slot.len) {
                size_t n = fwrite(slot.line + off, 1,
                        (size_t)slot.len - off, self.fp);
                if (n == 0) {
                    if (ferror(self.fp)) {
                        clearerr(self.fp);
                        self.write_errors++;
                    }
                    break;
                }
                off += n;
            }
        }

        if (self.fp) {
            ogs_time_t now = ogs_time_now();
            if (now - last_flush >= UDR_LAT_LOG_FLUSH_INTERVAL) {
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

void udr_lat_log_init(void)
{
    const char *env;

    if (self.inited)
        return;

    memset(&self, 0, sizeof(self));

    env = getenv("UDR_LAT_LOG_PATH");
    self.path = ogs_strdup(env ? env : UDR_LAT_LOG_DEFAULT_PATH);
    ogs_assert(self.path);

    self.fp = fopen(self.path, "a");
    if (!self.fp) {
        ogs_error("udr_lat_log_init: cannot open [%s]", self.path);
        ogs_free(self.path);
        self.path = NULL;
        return;
    }

    /* Large fully-buffered block: we flush explicitly on the writer thread, so
     * line buffering would only add syscalls, and a big block makes torn lines
     * far less likely. Lives on the writer FILE* only; never on the event loop. */
    self.log_buf = malloc(UDR_LAT_LOG_STREAM_BUF);
    if (self.log_buf)
        setvbuf(self.fp, self.log_buf, _IOFBF, UDR_LAT_LOG_STREAM_BUF);

    /* One long-lived ~256 MiB block allocated once at startup. Use libc
     * calloc(), not ogs_calloc(): open5gs' talloc pool rejects blocks this
     * large (same reason as lib/sbi/http-log.c). */
    self.ring = calloc(UDR_LAT_LOG_RING_SLOTS, sizeof(udr_lat_log_slot_t));
    if (!self.ring) {
        ogs_error("udr_lat_log_init: ring alloc failed");
        fclose(self.fp);
        self.fp = NULL;
        if (self.log_buf) {
            free(self.log_buf);
            self.log_buf = NULL;
        }
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

    ogs_info("udr_lat_log_init: writing [%s]", self.path);
}

void udr_lat_log_final(void)
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
        ogs_warn("udr_lat_log_final: dropped %llu records "
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

uint64_t udr_lat_log_dropped(void)
{
    return self.dropped + self.write_errors;
}

void udr_lat_log_emit(
        const ogs_sbi_response_snapshot_t *snapshot, ogs_time_t t5_tx)
{
    char ts1[40], ts2[40], ts3[40], ts4[40], ts5[40];
    char line[UDR_LAT_LOG_LINE_MAX];
    int len;

    if (!self.inited || !snapshot || !snapshot->valid)
        return;

    if (snapshot->t3_db_req == 0)
        return;

    format_ts(snapshot->t1_enq, ts1, sizeof(ts1));
    format_ts(snapshot->t2_deq, ts2, sizeof(ts2));
    format_ts(snapshot->t3_db_req, ts3, sizeof(ts3));
    format_ts(snapshot->t4_db_rsp, ts4, sizeof(ts4));
    format_ts(t5_tx, ts5, sizeof(ts5));

    len = ogs_snprintf(line, sizeof(line),
            "{\"src\":\"%s\",\"dst\":\"UDR\",\"method\":\"%s\",\"uri\":\"%s\","
            "\"t1_enq\":\"%s\",\"t2_deq\":\"%s\",\"t3_db_req\":\"%s\","
            "\"t4_db_rsp\":\"%s\",\"t5_tx\":\"%s\"}\n",
            snapshot->src, snapshot->method, snapshot->uri,
            ts1, ts2, ts3, ts4, ts5);

    ring_push(line, len);
}
