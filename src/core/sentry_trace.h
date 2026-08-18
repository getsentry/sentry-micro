/**
 * Trace context — what ties a device event to whatever else was happening.
 *
 * A crash on the device and a session replay in the app that caused it are the same
 * incident, and Sentry joins them on a shared `trace_id`. The device's whole job here is to
 * carry an id it was handed, attach it to what it emits, and then forget it.
 *
 * **A trace is a unit of work, not a lifetime.** One trace per boot is the tempting design
 * and it is wrong: it stays open for days, which the trace UI and the sampling model both
 * assume does not happen, and it welds unrelated interactions together. The bounded things
 * that *are* traces: an app-initiated operation, a boot, an OTA.
 *
 * So the device behaves like a backend. It adopts the context arriving with a request,
 * attaches it to any event raised while serving that request, and drops it on completion.
 * Holding the last-seen id indefinitely would link a panic three hours later to an
 * interaction it had nothing to do with — worse than no link at all, because nothing about
 * the result looks wrong. Same failure class as a mismatched `debug_id`.
 *
 * Portable and allocation-free: string parsing over fixed buffers, no clock, no device
 * calls. The id bytes come from the caller, which is what lets the host tests drive every
 * malformed-header case without a board.
 */
#ifndef SENTRY_MICRO_TRACE_H_INCLUDED
#define SENTRY_MICRO_TRACE_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 32 hex characters plus the terminator. Matches an event id: both are 16 bytes. */
#define SENTRY_MICRO_TRACE_ID_LEN 33
/** 16 hex characters plus the terminator. A span id is 8 bytes. */
#define SENTRY_MICRO_SPAN_ID_LEN 17

/**
 * One in-flight trace, as this device sees it.
 *
 * Every string is a fixed buffer rather than a pointer: the headers it was parsed from are
 * typically a stack buffer in whatever handled the request, and this outlives them.
 */
typedef struct {
    /** Shared with everyone in the trace. Empty when no trace is active. */
    char trace_id[SENTRY_MICRO_TRACE_ID_LEN];
    /** This device's own span within the trace. Never shared. */
    char span_id[SENTRY_MICRO_SPAN_ID_LEN];
    /** The span that called us, or empty when this device started the trace. */
    char parent_span_id[SENTRY_MICRO_SPAN_ID_LEN];
    /**
     * The caller's replay, from the `sentry-replay_id` baggage key. Empty when absent.
     *
     * Attaching it is what makes an issue link to the video of the interaction. It is
     * scoped to the request exactly like the trace id, and for the same reason.
     */
    char replay_id[SENTRY_MICRO_TRACE_ID_LEN];

    /** The upstream sampling decision. Honour it; do not make one up. */
    bool sampled;
    /** False when the header deferred the decision, i.e. carried no third field. */
    bool has_sampling_decision;
    /** False when nothing is in flight, which is the normal idle state. */
    bool active;
} sentry_trace_context_t;

/** Format 16 random bytes as a trace id. `out` needs `SENTRY_MICRO_TRACE_ID_LEN`. */
void sentry_trace_id_format(char *out, const uint8_t random_bytes[16]);

/** Format 8 random bytes as a span id. `out` needs `SENTRY_MICRO_SPAN_ID_LEN`. */
void sentry_span_id_format(char *out, const uint8_t random_bytes[8]);

/**
 * Adopt the trace described by a `sentry-trace` header, plus an optional `baggage`.
 *
 * Format is `<trace_id>-<span_id>[-<sampled>]`, and `span_id_bytes` becomes this device's
 * own span within it.
 *
 * Returns false and leaves `ctx` inactive if the header is malformed in any way — wrong
 * length, non-hex, missing the span. Rejecting wholesale is deliberate: adopting the
 * readable half of a garbled header would attach events to a trace that does not exist,
 * which is indistinguishable from working until somebody goes looking for the other end.
 */
bool sentry_trace_adopt_header(sentry_trace_context_t *ctx, const char *sentry_trace,
    const char *baggage, const uint8_t span_id_bytes[8]);

/**
 * Begin a trace this device is the origin of — a boot, an OTA, a scheduled task.
 *
 * No parent, and sampling is this device's decision because nobody else made one.
 */
void sentry_trace_begin(sentry_trace_context_t *ctx, const uint8_t trace_id_bytes[16],
    const uint8_t span_id_bytes[8], bool sampled);

/** Forget the active trace. Call when the operation finishes; see the header note. */
void sentry_trace_clear(sentry_trace_context_t *ctx);

/**
 * Write a `sentry-trace` header value for an outbound call, into `buf`.
 *
 * Returns the length written, or 0 if there is no active trace or `cap` is too small.
 * A deferred sampling decision is propagated as deferred — omitting the field rather than
 * guessing, so the far end can still decide.
 */
size_t sentry_trace_header_write(char *buf, size_t cap, const sentry_trace_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_TRACE_H_INCLUDED */
