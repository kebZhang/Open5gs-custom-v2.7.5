/*
 * See accesslog.h for the design rationale.
 */
#include "accesslog.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ACCESSLOG_QUEUE_CAP     (1 << 18)   /* 262144 records */
#define ENV_NGAP_PATH           "NGAP_LOG_PATH"
#define DEFAULT_NGAP_PATH       "/tmp/AMF_log.txt"
#define WRITER_BUF_SIZE         (1 << 20)   /* 1 MiB stdio buffer */

/* One queued record: a fully-formatted JSON line (built off the writer thread
 * so the writer stays trivial and formatting parallelizes with the hot path). */
typedef struct accesslog_rec_s {
    char *line; /* ogs_strdup/ogs_msprintf'd; freed by the writer */
} accesslog_rec_t;

static ogs_thread_t *writer_thread = NULL;
static ogs_queue_t  *log_queue = NULL;
static uint64_t      dropped = 0; /* updated with __atomic builtins */
static bool          running = false;

/* recv-time carrier: single AMF event thread, so a plain static is safe. */
static __thread ogs_time_t current_recv_time = 0;

void amf_recvtime_set(ogs_time_t t) { current_recv_time = t; }
ogs_time_t amf_recvtime_get(void) { return current_recv_time; }

static const char *env_or(const char *key, const char *def)
{
    const char *v = getenv(key);
    if (v && v[0] != '\0') return v;
    return def;
}

/* Render an ogs_time_t (microseconds since epoch, GMT) as an RFC3339-ish
 * UTC string with microsecond precision, e.g. 2026-06-29T12:34:56.123456Z.
 * Matches the sortable format used by the free5gc AMF_log. */
static void format_time(ogs_time_t t, char *buf, size_t len)
{
    struct tm tm;
    time_t sec = (time_t)ogs_time_sec(t);
    ogs_gmtime(sec, &tm);
    size_t n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
    if (n == 0) { buf[0] = '\0'; return; }
    ogs_snprintf(buf + n, len - n, ".%06lldZ",
            (long long)ogs_time_usec(t));
}

static void write_one(FILE *fp, accesslog_rec_t *rec)
{
    if (!rec) return;
    if (fp) {
        fputs(rec->line, fp);
        fputc('\n', fp);
    }
    ogs_free(rec->line);
    ogs_free(rec);
}

/* The single consumer. Owns the file exclusively, so writes never interleave.
 * Uses a short timed wait so it can notice shutdown (running==false) and then
 * drain every remaining record before exiting; ogs_queue_term() is intentionally
 * NOT used to wake it, because term makes pop discard still-queued records and
 * we want the trailing lines flushed. */
static void writer_loop(void *data)
{
    FILE *fp;
    accesslog_rec_t *rec;
    int rv;

    (void)data;

    fp = fopen(env_or(ENV_NGAP_PATH, DEFAULT_NGAP_PATH), "a");
    if (!fp)
        ogs_error("accesslog: cannot open log file; records will be dropped");
    else
        setvbuf(fp, NULL, _IOFBF, WRITER_BUF_SIZE);

    while (running) {
        /* Block up to 100ms; wakes promptly when records arrive, and lets us
         * re-check the running flag for a timely shutdown. */
        rv = ogs_queue_timedpop(log_queue, (void **)&rec,
                ogs_time_from_msec(100));
        if (rv == OGS_OK) {
            write_one(fp, rec);
            /* Batch: drain whatever is already queued without blocking. */
            while (ogs_queue_trypop(log_queue, (void **)&rec) == OGS_OK)
                write_one(fp, rec);
            if (fp) fflush(fp);
        }
        /* OGS_RETRY (timeout) / OGS_DONE: loop and re-check running. */
    }

    /* Shutdown: drain everything still queued so no trailing lines are lost. */
    while (ogs_queue_trypop(log_queue, (void **)&rec) == OGS_OK)
        write_one(fp, rec);

    if (fp) {
        fflush(fp);
        fclose(fp);
    }
}

void amf_accesslog_init(void)
{
    if (running) return;
    log_queue = ogs_queue_create(ACCESSLOG_QUEUE_CAP);
    ogs_assert(log_queue);
    running = true;
    writer_thread = ogs_thread_create(writer_loop, NULL);
    ogs_assert(writer_thread);
    ogs_info("accesslog: writer started, file=%s",
            env_or(ENV_NGAP_PATH, DEFAULT_NGAP_PATH));
}

void amf_accesslog_final(void)
{
    uint64_t n;

    if (!running) return;
    running = false; /* writer notices within one timedpop interval, drains, exits */
    ogs_thread_destroy(writer_thread); /* joins the writer */
    ogs_queue_destroy(log_queue);
    writer_thread = NULL;
    log_queue = NULL;

    n = __atomic_load_n(&dropped, __ATOMIC_RELAXED);
    if (n)
        ogs_warn("accesslog: %lld records dropped (queue full)", (long long)n);
}

uint64_t amf_accesslog_dropped(void)
{
    return __atomic_load_n(&dropped, __ATOMIC_RELAXED);
}

void amf_accesslog_ngap(const char *direction, const char *nas_type,
        const char *ue_id, ogs_time_t sctp_time)
{
    char ts[40];
    char *line;
    accesslog_rec_t *rec;
    int rv;

    if (!running) return;

    format_time(sctp_time, ts, sizeof(ts));

    /* Field values here (UL/DL, fixed type names, SUCI/SUPI) come from a
     * controlled character set with no JSON-special chars, so direct
     * formatting is safe and allocation-light. */
    line = ogs_msprintf(
            "{\"nf\":\"AMF\",\"dir\":\"%s\",\"nas_type\":\"%s\","
            "\"ue_id\":\"%s\",\"sctp_time\":\"%s\"}",
            direction ? direction : "",
            nas_type ? nas_type : "",
            ue_id ? ue_id : "",
            ts);
    if (!line) return;

    rec = ogs_calloc(1, sizeof(*rec));
    if (!rec) { ogs_free(line); return; }
    rec->line = line;

    rv = ogs_queue_trypush(log_queue, rec); /* never blocks the hot path */
    if (rv != OGS_OK) {
        __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
        ogs_free(rec->line);
        ogs_free(rec);
    }
}
