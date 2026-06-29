/*
 * accesslog: low-overhead asynchronous logging of NAS messages crossing the
 * AMF<->gNB SCTP boundary, for offline timestamp-based analysis.
 *
 * Design (mirrors the free5gc-tycustom accesslog package):
 *   - The hot path (NGAP/NAS handlers on the single AMF event thread) only
 *     formats a small JSON line and pushes it onto a bounded queue. It never
 *     touches the file and never blocks on I/O, so it cannot add latency to the
 *     UE registration / NAS data path.
 *   - A single dedicated writer thread drains the queue and appends to the file.
 *     Because there is exactly one writer, lines never interleave.
 *   - Output is JSON Lines (one JSON object per line) so records can be parsed
 *     and sorted by timestamp afterwards regardless of write order.
 *
 * File path is overridable with the NGAP_LOG_PATH env var
 * (default /tmp/AMF_log.txt).
 */
#ifndef AMF_ACCESSLOG_H
#define AMF_ACCESSLOG_H

#include "ogs-core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start/stop the background writer thread. Call init from amf_initialize() and
 * final from amf_terminate(). final() drains and flushes outstanding records so
 * the last lines are not lost on shutdown. */
void amf_accesslog_init(void);
void amf_accesslog_final(void);

/* Record one NAS message crossing the SCTP boundary.
 *   direction: "UL" (gNB->AMF) or "DL" (AMF->gNB)
 *   nas_type:  NAS message type name, e.g. "RegistrationRequest"
 *   ue_id:     SUPI/SUCI (may be NULL or "")
 *   sctp_time: SCTP read time (UL) or SCTP write time (DL), from ogs_time_now()
 * Never blocks; drops (and counts) the record if the queue is full. */
void amf_accesslog_ngap(const char *direction, const char *nas_type,
        const char *ue_id, ogs_time_t sctp_time);

/* Number of records dropped because the queue was full (sanity check). */
uint64_t amf_accesslog_dropped(void);

/* --- recv-time carrier ---------------------------------------------------
 *
 * The SCTP read time is captured at ngap_recv_handler() but the NAS message
 * type / SUCI are only known much later, in the gmm-handler. The AMF runs all
 * NGAP/NAS/GMM handling on one event thread (init.c: amf_main), so we carry the
 * read time in a thread-private static instead of threading it through ~10
 * handler signatures (the open5gs equivalent of free5gc's goroutine-local
 * recvtime). Set it right before dispatch, read it in the gmm-handler. */
void amf_recvtime_set(ogs_time_t t);
ogs_time_t amf_recvtime_get(void);

#ifdef __cplusplus
}
#endif

#endif /* AMF_ACCESSLOG_H */
