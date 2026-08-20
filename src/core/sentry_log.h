/**
 * Logs — a continuous console, not a story about one event.
 *
 * Every entry carries its own trace_id, captured when it is recorded rather than resolved
 * once for the whole batch the way a metric is: a line written during a real operation
 * belongs to that operation's trace, the same way a breadcrumb would. A line recorded while
 * idle carries an empty trace_id at record time; sentry_log_envelope_write() fills in a
 * fallback for those, the same "mint one for the batch" answer flush_metrics() already uses,
 * since an idle log line is not a causal claim the way a real trace attachment is.
 *
 * Fixed ring, no allocation, oldest evicted first: unlike the metrics table, there is no
 * running total here to protect by refusing new entries — for a continuous stream, the
 * newest line is worth more than the old one it replaces.
 */
#ifndef SENTRY_MICRO_LOG_H_INCLUDED
#define SENTRY_MICRO_LOG_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_envelope.h" /* sentry_level_t */
#include "sentry_trace.h" /* SENTRY_MICRO_TRACE_ID_LEN */

/**
 * Compile the SDK's log ring in (1, the default) or out (0).
 *
 * The ring below costs nothing on its own — these are plain functions over a struct you
 * would own, the same as a span. What costs something unconditionally is
 * `sentry_micro.c`'s singleton: it carries one `sentry_log_ring_t` (roughly 800 bytes at the
 * defaults) as a permanent `g_state` field, paid in every build whether or not firmware ever
 * calls `sentry_log()`, because nothing else there is optional either. Setting this to 0
 * removes that field along with `sentry_log()`, `sentry_logs_dropped_count()` and
 * `sentry_logs_truncated_count()`, so a build that does not want the console mirrored does
 * not carry the ring that would have held it.
 *
 * This header's own ring type stays available either way — a caller who wants a log ring
 * with different lifetime than the singleton's can still build one directly.
 *
 *     build_flags = -D SENTRY_MICRO_LOGS_ENABLED=0
 */
#ifndef SENTRY_MICRO_LOGS_ENABLED
#    define SENTRY_MICRO_LOGS_ENABLED 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Entries held at once.
 *
 * Lower than SENTRY_MICRO_MAX_METRICS (8): a log entry's JSON is heavier per item than a
 * metric's — a full body plus its own attributes, against a metric's bare name and number
 * — so the same slot count does not fit the same envelope budget. Eight entries at maximum
 * body length, each carrying its worst-case attributes, need more than
 * SENTRY_MICRO_ENVELOPE_BUFFER_BYTES; six is what stays under it even in that worst case,
 * not a number chosen for its own sake.
 */
#ifndef SENTRY_MICRO_MAX_LOGS
#    define SENTRY_MICRO_MAX_LOGS 6
#endif

/**
 * Bytes a single entry's body holds, including the terminator. Matches a conventional
 * terminal line width. A longer message is truncated to fit, not dropped — a shortened line
 * the operator can still read beats losing it entirely.
 */
#ifndef SENTRY_MICRO_LOG_BODY_LEN
#    define SENTRY_MICRO_LOG_BODY_LEN 81
#endif

typedef struct {
    char body[SENTRY_MICRO_LOG_BODY_LEN];
    /** Empty when recorded with no trace active; resolved to a fallback at write time. */
    char trace_id[SENTRY_MICRO_TRACE_ID_LEN];
    uint64_t uptime_us;
    sentry_level_t level;
    /** Whether `body` is shorter than what was actually meant to be logged. */
    bool truncated;
    bool used;
} sentry_log_entry_t;

typedef struct {
    sentry_log_entry_t entries[SENTRY_MICRO_MAX_LOGS];
    /** Index of the oldest entry. */
    uint8_t head;
    uint8_t count;
    /** Entries evicted to make room before they were ever sent. Reported, never silent. */
    uint16_t dropped;
} sentry_log_ring_t;

/** Empty the ring. */
void sentry_log_ring_reset(sentry_log_ring_t *ring);

/**
 * Record one line, evicting the oldest entry first if the ring is already full.
 *
 * `trace_id` is copied verbatim — pass an empty string when nothing is active, resolved to
 * a fallback only when the ring is serialised. `body` is truncated to
 * SENTRY_MICRO_LOG_BODY_LEN - 1 characters if longer; formatting the message is the caller's
 * job, same split as everywhere else in this SDK that takes a finished string.
 *
 * `truncated` is the caller's own knowledge that `body` is shorter than what was meant to be
 * logged — typically vsnprintf()'s return value compared against the buffer it formatted
 * into, information this call cannot recover once `body` already reflects the loss. This
 * call ORs that with its own truncation of `body` into the ring's fixed field, so a caller
 * that has not already sized `body` to fit still gets a correct answer.
 *
 * Returns true when an existing entry was evicted to make room for this one. The new line
 * is always recorded either way — nothing about this call ever rejects it — so the return
 * value exists only so a caller can count what was lost, the same role
 * sentry_metrics_count()/gauge()'s bool return plays for a name that did not fit.
 */
bool sentry_log_ring_push(sentry_log_ring_t *ring, sentry_level_t level, const char *trace_id,
    uint64_t uptime_us, const char *body, bool truncated);

/** True when there is nothing worth sending, which is the common case between flushes. */
bool sentry_log_ring_empty(const sentry_log_ring_t *ring);

/**
 * Write a complete envelope carrying every entry in the ring, oldest first.
 *
 * `fallback_trace_id` fills in any entry that had no trace active when it was recorded.
 * `device_id` is attached to every entry as an attribute — the correlation axis that is
 * always available, independent of whichever trace_id an idle-recorded entry ends up with.
 *
 * `now_uptime_us` and `now_unix_us` are both read at the same instant (the flush moment) and
 * anchor each entry's stored monotonic uptime to a wall-clock timestamp — the same
 * derivation sentry_transaction_start_unix_us() does for a span's start, done per entry here
 * instead of once.
 *
 * Returns 0 when the ring is empty or `now_unix_us` is 0: a device that has not been told
 * the date holds its logs rather than sending them anchored to the epoch, the same as
 * metrics — a longer covered interval is still true, unlike a mistimed line.
 */
size_t sentry_log_envelope_write(char *buf, size_t cap, const sentry_log_ring_t *ring,
    const char *fallback_trace_id, const char *device_id, uint64_t now_uptime_us,
    uint64_t now_unix_us);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_LOG_H_INCLUDED */
