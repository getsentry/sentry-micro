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
 * second occupies one slot. A name beyond this is dropped and counted rather than evicting
 * one that is already accumulating, because losing a running total silently is worse than
 * not starting a new one.
 *
 * Ten, not the eight this was, and not a round number chosen for its own sake. Eight was
 * set on the claim that it "covers the numbers a device actually has", which turned out to
 * be wrong on the first real integration: a controller reported boot, setup_ms, free_heap,
 * min_free_heap, loop_stack_free, uptime, fps_x10, ble_disconnect — and then
 * `sentry_logs_dropped_count()` and `sentry_logs_truncated_count()`, which this SDK's own
 * API invites you to report. A default that cannot accommodate the SDK's own suggested
 * usage is the wrong default.
 *
 * Raising it is not free, and the cost is in the envelope rather than in RAM (a slot is
 * ~24 bytes). Every item carries its own 32-character trace_id and timestamp, so an item
 * costs ~142 bytes with a short name and never less than ~114. Measured against
 * SENTRY_MICRO_ENVELOPE_BUFFER_BYTES at 2048, with short names: ten items encode to 1,555
 * bytes, thirteen to 1,976, and fourteen to 2,117 — over. Longer names move that cliff
 * closer, and eight 128-character names already exceed the budget on their own.
 *
 * So the headroom at ten is real but finite, and it is name-length-sensitive. An
 * over-budget batch is dropped and reported rather than wedging the table (see
 * flush_metrics()), and the compile-time check below refuses only the values that cannot work at
 * any name length.
 */
#ifndef SENTRY_MICRO_MAX_METRICS
#    define SENTRY_MICRO_MAX_METRICS 10
#endif

/**
 * Why every item repeats the same trace_id, and why that cannot be batched away.
 *
 * At ten metrics, `"trace_id":"<32 hex>"` accounts for ~460 of ~1,555 bytes — near enough a
 * third of the envelope spent on one string repeated ten times. Since flush_metrics()
 * resolves a single trace id for the whole batch (minting one when the device is idle,
 * which is its normal state), the obvious saving is to state it once and let ingest expand
 * it.
 *
 * Ingest does not allow that. The trace metric spec makes `trace_id` **REQUIRED on every
 * metric payload**, and defines no shared, common or default attributes across the items in
 * a `trace_metric` container — the `items` array is a batching convenience, not a place to
 * factor out repeated fields. Mixing metrics from different traces in one item is
 * explicitly permitted, which is precisely why the field cannot be hoisted.
 * See https://develop.sentry.dev/sdk/telemetry/metrics/.
 *
 * Written down so the next person to notice those repeated bytes does not have to
 * re-investigate. If the spec ever grows shared attributes, this is the single biggest
 * lever on how many metrics fit a constrained transport — it would roughly double it.
 */

/**
 * Refuses a SENTRY_MICRO_MAX_METRICS that cannot fit the envelope at *any* name length.
 *
 * A floor check, deliberately not a guarantee: the real encoded size depends on how long
 * the names are, which is a runtime property. This catches only the values that are wrong
 * before a single character of name is written — the case that previously failed silently
 * at runtime, on a device, by dropping every batch.
 */
#define SENTRY_MICRO_METRIC_ITEM_MIN_BYTES 114
#define SENTRY_MICRO_METRIC_ENVELOPE_FIXED_BYTES 131

/* A negative-width array rather than static_assert: this SDK builds as C99, where
 * static_assert does not exist. The error a compiler prints for it names this type, which is
 * why the type is named the way it is. */
typedef char sentry_micro_max_metrics_must_fit_the_envelope_buffer
    [(SENTRY_MICRO_METRIC_ENVELOPE_FIXED_BYTES
             + (SENTRY_MICRO_MAX_METRICS)*SENTRY_MICRO_METRIC_ITEM_MIN_BYTES
         <= SENTRY_MICRO_ENVELOPE_BUFFER_BYTES)
            ? 1
            : -1];

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
