/**
 * Application Metrics — the numbers that belong to no operation.
 *
 * A span attribute enriches a trace, and needs an operation to hang off. The numbers a
 * device most wants to report have none: free heap over a week, WiFi RSSI, frame time,
 * brownout counts. On a controller whose only operations are OTA, config writes and boot,
 * there is otherwise nowhere to put "heap has been trending down since Tuesday", which is
 * the most useful thing a fleet of them can say.
 *
 * These are a separate Sentry product from span metrics, on a separate envelope item, and
 * deliberately unaffected by trace sampling.
 *
 * **Recording does not send.** That is the whole point on this hardware. `transaction_finish()`
 * posts inline, which blocks the loop task rendering the LEDs — so a path that runs several
 * times a second cannot be traced at any sampling rate. A counter adds to a fixed table and
 * returns, and the table rides the next flush that was happening anyway. This is what makes
 * the hot path measurable.
 *
 * No allocation. A clock *is* required — every metric carries a timestamp, which the field
 * table in Sentry's spec marks required even though its example payloads omit it. Building
 * to those examples produced metrics that ingest accepted at the edge and dropped, with
 * nothing anywhere reporting it.
 */
#ifndef SENTRY_MICRO_METRICS_H_INCLUDED
#define SENTRY_MICRO_METRICS_H_INCLUDED

#include "sentry_boot.h"

/**
 * Compile the SDK's metrics table in (1, the default) or out (0).
 *
 * The table below costs nothing on its own — plain functions over a struct you would own,
 * the same as a span. What costs something unconditionally is `sentry_micro.c`'s singleton:
 * it carries one `sentry_metrics_t` (roughly 250 bytes at the defaults) as a permanent
 * `g_state` field, paid in every build whether or not firmware ever calls
 * `sentry_metric_count()` / `sentry_metric_gauge()`. Setting this to 0 removes that field
 * along with those two functions and `sentry_metrics_dropped_count()`, so a build that never
 * counts anything does not carry the table that would have held it.
 *
 * This header's own table type stays available either way.
 *
 *     build_flags = -D SENTRY_MICRO_METRICS_ENABLED=0
 */
#ifndef SENTRY_MICRO_METRICS_ENABLED
#    define SENTRY_MICRO_METRICS_ENABLED 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Distinct metric names held at once.
 *
 * Aggregated in place, so this bounds *names*, not calls — a counter hit a thousand times a
 * second occupies one slot. Eight covers the numbers a device actually has; a ninth name is
 * dropped and counted rather than evicting one that is already accumulating, because losing
 * a running total silently is worse than not starting a new one.
 */
#ifndef SENTRY_MICRO_MAX_METRICS
#    define SENTRY_MICRO_MAX_METRICS 8
#endif

typedef enum {
    /** Monotonic total since the last flush, e.g. brownouts, BLE disconnects. */
    SENTRY_METRIC_COUNTER = 0,
    /** Latest reading, e.g. free heap, RSSI. */
    SENTRY_METRIC_GAUGE,
} sentry_metric_type_t;

typedef struct {
    /** Not copied: pass a literal. Names are compile-time in every real use. */
    const char *name;
    /** Optional, e.g. `"byte"`, `"millisecond"`. Not copied either. */
    const char *unit;
    sentry_metric_type_t type;
    int64_t value;
    bool used;
} sentry_metric_t;

typedef struct {
    sentry_metric_t items[SENTRY_MICRO_MAX_METRICS];
    /** Names that did not fit since the last flush. Reported, never silent. */
    uint16_t dropped;
} sentry_metrics_t;

/** Empty the table. Called at init and after a successful flush. */
void sentry_metrics_reset(sentry_metrics_t *metrics);

/**
 * Add to a counter, creating it if this name is new.
 *
 * Returns false only when the table is full of other names, in which case `dropped` goes up.
 */
bool sentry_metrics_count(
    sentry_metrics_t *metrics, const char *name, int64_t delta, const char *unit);

/** Set a gauge to its latest reading. Same table, same full-table behaviour. */
bool sentry_metrics_gauge(
    sentry_metrics_t *metrics, const char *name, int64_t value, const char *unit);

/** True when there is nothing worth sending, which is the common case between flushes. */
bool sentry_metrics_empty(const sentry_metrics_t *metrics);

/**
 * Write a complete envelope carrying every metric in the table.
 *
 * `trace_id` and `timestamp` are both required on every metric by the protocol. The trace
 * must come from the current propagation context — pass the active trace, or one minted for
 * the batch. `now_unix_us` stamps every metric in the batch with the moment it was flushed,
 * which is the honest reading of an aggregate describing the interval that just ended.
 *
 * Returns 0 when `now_unix_us` is 0, because a metric without a timestamp is dropped
 * server-side after the radio has already been paid for.
 *
 * Returns bytes needed excluding the NUL; >= `cap` means nothing usable was written, and 0
 * means there was nothing to send.
 */
size_t sentry_metrics_envelope_write(char *buf, size_t cap, const sentry_metrics_t *metrics,
    const char *trace_id, uint64_t now_unix_us);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_METRICS_H_INCLUDED */
