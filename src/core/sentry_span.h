/**
 * Spans: what the device *did*, not only what went wrong with it.
 *
 * Trace context (`sentry_trace.h`) already lets a device event join the caller's trace, so
 * a crash lands on the same timeline as the app action that provoked it. What the device
 * cannot yet do is contribute anything of its own, which means it appears in the Traces
 * view only when it fails.
 *
 * A transaction fixes that: one unit of work, with child spans for its phases, carrying
 * real durations. It is also where metrics live — the standalone metrics API is gone and
 * numeric attributes on spans are the current path — so this is the thing everything else
 * hangs off.
 *
 * **Scoped to app-initiated operations, deliberately.** A boot transaction would start
 * before anything has told the device what time it is, and on a BLE-only device that may
 * never happen at all on a given power cycle. An operation the app asked for runs while
 * the app is connected, which is exactly when the clock is fresh.
 *
 * Fixed storage, no allocation, and a hard span cap: a device that runs out of span slots
 * says so in the transaction rather than quietly reporting a shorter one, because a trace
 * that looks complete and is not is worse than one that admits it was truncated.
 */
#ifndef SENTRY_MICRO_SPAN_H_INCLUDED
#define SENTRY_MICRO_SPAN_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Child spans one transaction can hold.
 *
 * Four, because the transaction lives on the caller's stack and Arduino's loop task has
 * 8 KB of it in total — which a TLS handshake already wants several KB of. Measured on
 * ESP32:
 *
 *     spans   sizeof(sentry_transaction_t)   + 2 KB envelope buffer = peak stack
 *       4                688 B                          2.7 KB
 *       8              1,200 B                          3.2 KB
 *      16              2,224 B                          4.2 KB
 *
 * Four covers "decode, validate, apply, ack", which is what a device operation actually
 * is. Raising it is a decision with a number attached rather than a guess, and running
 * out is reported (`dropped_spans`, and a `spans_dropped` tag) rather than hidden.
 *
 * The whole transaction also has to fit in `SENTRY_MICRO_ENVELOPE_BUFFER_BYTES` at about
 * 150 bytes of JSON per span, so raise the two together or the envelope will not fit.
 */
#ifndef SENTRY_MICRO_MAX_SPANS
#    define SENTRY_MICRO_MAX_SPANS 4
#endif

/** Measurements a single span can carry — free heap, RSSI, frame time. */
#ifndef SENTRY_MICRO_MAX_SPAN_ATTRS
#    define SENTRY_MICRO_MAX_SPAN_ATTRS 4
#endif

typedef struct {
    const char *key;
    /* Integer, not double: free heap, RSSI, frame time in microseconds and battery in
     * millivolts are all whole numbers, and printf's float support is an opt-in linker
     * flag on this target that firmware routinely leaves off. */
    int64_t value;
} sentry_span_attr_t;

typedef struct {
    /** What kind of work this is, e.g. `"ble.write"`. Shown as the span's label. */
    const char *op;
    /** Free-form detail, e.g. the characteristic being written. Optional. */
    const char *description;

    char span_id[SENTRY_MICRO_SPAN_ID_LEN];

    /**
     * Monotonic, not wall clock. The duration comes from these and is therefore always
     * correct; only the transaction's position on the timeline needs a real clock.
     */
    uint64_t start_uptime_us;
    uint64_t end_uptime_us;
    bool finished;

    sentry_span_attr_t attrs[SENTRY_MICRO_MAX_SPAN_ATTRS];
    uint8_t attr_count;
} sentry_span_t;

typedef struct {
    /** Groups the transaction in Sentry, e.g. `"set-colour"`. Required. */
    const char *name;
    /** e.g. `"device.operation"`. Optional. */
    const char *op;

    /** Copied, so the transaction still describes its own trace after the trace is released. */
    sentry_trace_context_t trace;

    uint64_t start_uptime_us;
    uint64_t end_uptime_us;
    /** Wall clock at finish. The start is derived by subtracting the monotonic elapsed time. */
    uint64_t end_unix_us;

    sentry_span_t spans[SENTRY_MICRO_MAX_SPANS];
    uint8_t span_count;
    /** Spans that did not fit. Reported, never silently discarded. */
    uint16_t dropped_spans;

    bool active;
} sentry_transaction_t;

/** Begin a transaction against `trace`. `name` must outlive it; nothing is copied. */
void sentry_transaction_begin(sentry_transaction_t *txn, const sentry_trace_context_t *trace,
    const char *name, const char *op, uint64_t now_uptime_us);

/**
 * Open a child span, or NULL when the transaction is full or not running.
 *
 * A NULL return is counted in `dropped_spans` and surfaces on the transaction, so losing
 * spans is visible rather than inferred from a suspiciously tidy trace.
 */
sentry_span_t *sentry_span_open(sentry_transaction_t *txn, const char *op, const char *description,
    const uint8_t span_id_bytes[8], uint64_t now_uptime_us);

/** Close a span. Closing one twice keeps the first end time, which is the honest one. */
void sentry_span_close(sentry_span_t *span, uint64_t now_uptime_us);

/**
 * Attach a numeric attribute — free heap, RSSI, frame time.
 *
 * Silently ignored past `SENTRY_MICRO_MAX_SPAN_ATTRS`: unlike a dropped span, a dropped
 * measurement does not change the shape of the trace.
 */
void sentry_span_set_number(sentry_span_t *span, const char *key, int64_t value);

/**
 * Close the transaction. `now_unix_us` may be 0 when the clock has never been set.
 *
 * Returns false in that case, and the caller must drop the transaction: a duration has no
 * server-side substitute, and emitting one anchored to nothing would put real work at the
 * epoch. An error would still have been reportable — this restriction is specific to
 * anything with a duration.
 */
bool sentry_transaction_end_at(
    sentry_transaction_t *txn, uint64_t now_uptime_us, uint64_t now_unix_us);

/** Unix microseconds when the transaction began, derived from the monotonic elapsed time. */
uint64_t sentry_transaction_start_unix_us(const sentry_transaction_t *txn);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_SPAN_H_INCLUDED */
