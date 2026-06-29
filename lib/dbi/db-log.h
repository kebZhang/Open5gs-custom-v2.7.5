/*
 * db-log.h -- asynchronous NF<->MongoDB access logging (TYcustom instrumentation)
 *
 * Records, for every MongoDB operation issued through lib/dbi (the ogs_dbi_*
 * functions), one line into DB_log.txt (JSON Lines). It is the DB-side
 * counterpart of lib/sbi/http-log.c and uses the SAME asynchronous mechanism so
 * the two logs can be analysed together by timestamp.
 *
 * CORE REQUIREMENT: logging must NOT add latency to UE registration. The emit
 * function runs on the NF's event-loop thread (the same thread that called the
 * ogs_dbi_* function). It only: captures the two ogs_time_now() values the
 * caller already took around the synchronous mongoc call, formats one line, and
 * memcpy's it into a single-producer ring buffer (the mutex is held only long
 * enough to move an index -- never for file I/O). A dedicated background writer
 * thread drains the ring and writes the file. If the ring is full the record is
 * DROPPED (counted) -- the data path never blocks. Write order is not preserved;
 * offline analysis re-sorts by timestamp.
 *
 * Because each open5gs NF runs as its own pod, each pod opens and writes its own
 * DB_log.txt.
 *
 * Output path: env DB_LOG_PATH, default /tmp/DB_log.txt.
 *
 * NF name: lib/dbi does NOT depend on lib/sbi, so it cannot ask ogs_sbi_self()
 * which NF it runs as. Each NF passes its own name to ogs_db_log_init("UDR").
 */

#if !defined(OGS_DBI_INSIDE) && !defined(OGS_DBI_COMPILATION)
#error "This header cannot be included directly."
#endif

#ifndef OGS_DBI_DB_LOG_H
#define OGS_DBI_DB_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the background writer thread and open DB_log.txt.
 *   nf_name : this NF's name ("UDR","PCF","HSS","PCRF",...), written as "nf".
 * Safe to call more than once; only the first call has effect. Call it right
 * after ogs_dbi_init() in each NF's init path.
 */
void ogs_db_log_init(const char *nf_name);

/* Stop the writer thread and flush+close the file. Call near ogs_dbi_final(). */
void ogs_db_log_final(void);

/*
 * Emit one DB_log line for a single (synchronous) MongoDB operation.
 *   resource    : collection name ("subscriber").
 *   subresource : finer SBI/semantic tag so repeated reads of the same
 *                 collection can be told apart (e.g. "am-data",
 *                 "smf-selection-subscription-data", "authentication-subscription",
 *                 "authentication-status"); may be NULL/"" when not applicable.
 *   operation   : DB op type ("GetOne","PutOne","IncrSqn",...).
 *   ueid        : the SUPI/IMSI this op is for (may be NULL/"").
 *   req_time    : ogs_time_now() captured right BEFORE the mongoc call.
 *   resp_time   : ogs_time_now() captured right AFTER the mongoc call returned.
 * All work is in-memory; this never blocks on I/O.
 */
void ogs_db_log_emit(const char *resource, const char *subresource,
        const char *operation, const char *ueid,
        ogs_time_t req_time, ogs_time_t resp_time);

/* Number of records dropped because the ring was full (0 == writer keeps up). */
uint64_t ogs_db_log_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* OGS_DBI_DB_LOG_H */
