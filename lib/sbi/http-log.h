/*
 * http-log.h -- asynchronous SBI HTTP access logging (TYcustom instrumentation)
 *
 * Records, for every SBI (HTTP/2) message, four views into HTTP_log.txt
 * (JSON Lines):
 *
 *   REQ_TX : this NF is sending a request   (client.c, before libcurl)
 *   REQ_RX : this NF received a request     (path.c server handler entry)
 *   RSP_TX : this NF is sending a response  (server.c send_response entry)
 *   RSP_RX : this NF received a response    (client.c, on CURLMSG_DONE)
 *
 * CORE REQUIREMENT: logging must NOT add latency to UE registration. Each
 * open5gs NF is a single event-loop thread; the four emit points run on that
 * thread, so they must never block. They only: capture ogs_time_now(), format
 * one line, and memcpy it into a single-producer ring buffer (no file I/O, no
 * lock). A dedicated background writer thread drains the ring and writes the
 * file. If the ring is full the record is DROPPED (counted) -- never blocks.
 * Write order is not preserved; the offline analysis re-sorts by timestamp and
 * groups by (ue_id, method, uri).
 *
 * Output path: env HTTP_LOG_PATH, default /tmp/HTTP_log.txt.
 */

#ifndef OGS_SBI_HTTP_LOG_H
#define OGS_SBI_HTTP_LOG_H

/*
 * This header is included from ogs-sbi.h AFTER the request/response types are
 * defined (sbi/message.h), so we only need their forward declarations here and
 * must NOT include ogs-sbi.h back (that would be circular).
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ogs_sbi_request_s ogs_sbi_request_t;
typedef struct ogs_sbi_response_s ogs_sbi_response_t;

/* event names */
#define OGS_HTTP_LOG_REQ_TX "REQ_TX"
#define OGS_HTTP_LOG_REQ_RX "REQ_RX"
#define OGS_HTTP_LOG_RSP_TX "RSP_TX"
#define OGS_HTTP_LOG_RSP_RX "RSP_RX"

/* Start/stop the background writer thread. Call from ogs_sbi_context_init/final. */
void ogs_http_log_init(void);
void ogs_http_log_final(void);

/*
 * Emit one log line for a request-side event (REQ_TX / REQ_RX).
 *   ue_id : may be NULL/"" -- when the URI carries the UE id, leave it empty
 *           and extract it offline from the URI. For the body-only requests
 *           (nausf ue-authentications / npcf policies) the caller may pass a
 *           sniffed ue_id.
 */
void ogs_http_log_request(const char *event,
        ogs_sbi_request_t *request, const char *ue_id);

/*
 * Emit one log line for a response-side event (RSP_TX / RSP_RX).
 *   ue_id  : may be NULL/"" (see above).
 *   ctx_id : may be NULL -- when set (auth-response case) it is written as the
 *            "ctx_id" field so the offline step can build a ctx_id->ue_id map.
 */
void ogs_http_log_response(const char *event,
        ogs_sbi_response_t *response, const char *ue_id, const char *ctx_id);

/*
 * Emit the RSP_TX line. The outgoing response has no h.uri/h.method of its own
 * (ogs_sbi_build_response() leaves them unset), so method/uri/dst are taken
 * from the originating server request (req, may be NULL). ue_id is left empty;
 * the offline step attributes it by pairing on the request's (method, uri-path)
 * with the REQ_RX/REQ_TX lines.
 *
 */
void ogs_http_log_rsp_tx(
        ogs_sbi_response_t *response, ogs_sbi_request_t *req);

/* Number of records dropped because the ring was full (0 == writer keeps up). */
uint64_t ogs_http_log_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* OGS_SBI_HTTP_LOG_H */
