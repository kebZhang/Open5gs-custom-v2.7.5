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
    char *path;

    uint64_t dropped;
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
            fwrite(slot.line, 1, slot.len, self.fp);
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
    self.path = ogs_strdup(env ? env : DB_LOG_DEFAULT_PATH);
    ogs_assert(self.path);

    self.fp = fopen(self.path, "a");
    if (!self.fp) {
        ogs_error("ogs_db_log_init: cannot open [%s]", self.path);
        ogs_free(self.path);
        self.path = NULL;
        return;
    }

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
        fclose(self.fp);
        self.fp = NULL;
    }

    if (self.ring) {
        free(self.ring); /* paired with calloc() in init */
        self.ring = NULL;
    }

    ogs_thread_cond_destroy(&self.cond);
    ogs_thread_mutex_destroy(&self.mutex);

    if (self.dropped)
        ogs_warn("ogs_db_log_final: dropped %llu records",
                (unsigned long long)self.dropped);

    if (self.path) {
        ogs_free(self.path);
        self.path = NULL;
    }

    self.inited = false;
}

uint64_t ogs_db_log_dropped(void)
{
    return self.dropped;
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
