/*
 * udr-lat-log.h -- asynchronous per-request UDR latency logging
 *                  (TYcustom instrumentation)
 *
 * Records, for every inbound HTTP request that the UDR actually serves against
 * MongoDB, ONE line into UDR_log.txt containing the request's identity
 * (src NF, dst NF, URI, method) and five lifecycle timestamps:
 *
 *   t1_enq    : request pushed onto the NF event queue
 *               (lib/sbi/path.c, just before ogs_queue_push)
 *   t2_deq    : request popped from the queue and dispatched
 *               (src/udr/udr-sm.c, OGS_EVENT_SBI_SERVER entry)
 *   t3_db_req : just before the FIRST mongoc call for this request
 *               (src/udr/nudr-handler.c, via UDR_LAT_DB_ENTER)
 *   t4_db_rsp : just after the LAST mongoc call for this request returned
 *               (src/udr/nudr-handler.c, via UDR_LAT_DB_LEAVE)
 *   t5_tx     : just after the response completed HTTP/2 serialization and
 *               was appended to the connection's userspace write_queue
 *               (lib/sbi/server.c, after server_send_response returns).
 *               The real socket write happens later in the POLLOUT callback.
 *
 * CORE REQUIREMENT: logging must NOT add latency to the request it measures.
 * UDR is a single event-loop thread; the five capture points and the emit point
 * all run on that thread, so they must never block. They only: read
 * ogs_time_now() (the t1..t4 captures are stored on ogs_sbi_request_t, no I/O),
 * format one line, and memcpy it into a single-producer ring buffer. A dedicated
 * background writer thread drains the ring and appends to the file. Ring full =>
 * the record is DROPPED (counted), the event loop never waits. Write order is
 * NOT preserved; offline analysis re-sorts. Memory is generous on purpose (the
 * machine has plenty of RAM, as required).
 *
 * Mechanism is intentionally identical to lib/sbi/http-log.c so the two logs can
 * be analysed together by timestamp.
 *
 * Output path: env UDR_LAT_LOG_PATH, default /tmp/UDR_log.txt. When HOSTNAME is
 * set (Kubernetes pod name), a "_<pod>" suffix is inserted before the ".txt"
 * extension, e.g. /tmp/UDR_log_udr-0.txt, so 10 UDR pods write distinct files.
 */

#ifndef UDR_LAT_LOG_H
#define UDR_LAT_LOG_H

#include "ogs-sbi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start/stop the background writer thread. Call from udr init/terminate. */
void udr_lat_log_init(void);
void udr_lat_log_final(void);

/*
 * Emit one UDR_log line for a request that hit the DB.
 *   snapshot: immutable copy of request identity and t1..t4, captured before
 *             nghttp2 serialization so it remains valid if the stream closes.
 *   t5_tx   : ogs_time_now() captured after HTTP/2 serialization/userspace
 *             write_queue enqueue, before the later POLLOUT socket write.
 * No-op (nothing emitted) unless t3_db_req was set, i.e. the request actually
 * issued at least one DB call. All work is in-memory; never blocks on I/O.
 */
void udr_lat_log_emit(
        const ogs_sbi_response_snapshot_t *snapshot, ogs_time_t t5_tx);

/* Number of records dropped because the ring was full (0 == writer keeps up). */
uint64_t udr_lat_log_dropped(void);

/*
 * Record the DB-interaction span on the request.
 *   UDR_LAT_DB_ENTER: set t3_db_req only if not yet set (FIRST call wins).
 *   UDR_LAT_DB_LEAVE: always overwrite t4_db_rsp (LAST call wins).
 * Wrap each ogs_dbi_* call in nudr-handler.c with these. Safe with req==NULL.
 */
static ogs_inline void udr_lat_db_enter(ogs_sbi_request_t *req)
{
    if (req && req->tycustom_lat.t3_db_req == 0)
        req->tycustom_lat.t3_db_req = ogs_time_now();
}
static ogs_inline void udr_lat_db_leave(ogs_sbi_request_t *req)
{
    if (req)
        req->tycustom_lat.t4_db_rsp = ogs_time_now();
}

/*
 * Convenience: in nudr-handler.c every handler has `stream` in scope. These
 * resolve stream -> request and bracket a DB call.
 *
 *   UDR_LAT_DB(stream, rv = ogs_dbi_xxx(...));
 *
 * captures t3 (first) before and t4 (last) after the wrapped statement.
 */
#define UDR_LAT_DB(stream, stmt) do { \
    ogs_sbi_request_t *_lat_req = ogs_sbi_request_from_stream(stream); \
    udr_lat_db_enter(_lat_req); \
    stmt; \
    udr_lat_db_leave(_lat_req); \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* UDR_LAT_LOG_H */
