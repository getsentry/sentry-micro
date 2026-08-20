#include "sentry_micro.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * One statically-allocated instance of SDK state.
 *
 * A singleton is not laziness here — it is the point. There is exactly one device, one DSN,
 * and one crash to report, and the crash handler must be able to reach this state without
 * dereferencing anything that could have been corrupted or freed. Static storage means the
 * SDK costs a fixed, knowable number of bytes of RAM that shows up in the map file, which
 * matters when the whole budget is 320 KB.
 */
typedef struct {
    sentry_options_t options;
    sentry_dsn_t dsn;
    sentry_device_info_t device;
    const sentry_transport_t *transport;
    char envelope_url[SENTRY_MICRO_MAX_URL_LEN];
    char auth_header[SENTRY_MICRO_MAX_AUTH_LEN];
    bool enabled;

    sentry_buffer_t buffer;
    bool buffering;
    /**
     * Uptime, in ms, before which no delivery should be attempted.
     *
     * Set from a server-supplied Retry-After. Honouring it is the difference between a
     * boot-looping device backing off politely and one hammering ingest with the same
     * crash until the whole project is rate-limited for everyone using it.
     */
    uint64_t retry_after_uptime_ms;

    /** Local ceiling on captured messages; see sentry_throttle.h for why it is not optional. */
    sentry_throttle_t throttle;

    /** The operation currently being served, if any. See core/sentry_trace.h. */
    sentry_trace_context_t trace;

    /**
     * Numbers accumulated between flushes. About 200 bytes, and unlike a transaction this
     * has to live across calls — a counter that reset every time nobody was looking would
     * count nothing. Absent entirely when built with SENTRY_MICRO_METRICS_ENABLED=0.
     */
#if SENTRY_MICRO_METRICS_ENABLED
    sentry_metrics_t metrics;
    uint32_t metrics_dropped;
#endif

    /**
     * Console lines accumulated between flushes. See core/sentry_log.h. Absent entirely
     * when built with SENTRY_MICRO_LOGS_ENABLED=0.
     */
#if SENTRY_MICRO_LOGS_ENABLED
    sentry_log_ring_t log_ring;
    /**
     * Lines evicted from the ring since sentry_init(), persistent across flushes.
     *
     * Separate from log_ring.dropped on purpose, same split as metrics/metrics_dropped:
     * the ring's own counter describes the batch about to be sent and is cleared with it,
     * so a lifetime counter has to live outside anything a flush resets.
     */
    uint32_t logs_dropped;
    /**
     * Lines recorded shorter than intended, since sentry_init() — see sentry_log().
     *
     * Note this counts a *record-time* loss, unlike logs_dropped's second cause, which is a
     * flush-time one: a body can be recorded untruncated and still make its batch unsendable
     * once escaped.
     */
    uint32_t logs_truncated;
#endif

    /**
     * The trace the *previous* boot died inside, recovered from RTC memory.
     *
     * Kept apart from the active one on purpose. The app that comes to collect a crash
     * report connects and immediately offers a new trace, so if the two shared a slot the
     * panic would be attached to the connection that came to fetch it — a wrong link that
     * renders identically to a right one. Separating them makes the ordering of "adopt"
     * and "report the last boot" stop mattering, which is better than documenting it.
     */
    sentry_trace_context_t crash_trace;
} sentry_state_t;

static sentry_state_t g_state;
static sentry_logger_fn g_logger;

static void debug_log(const char *fmt, ...)
{
    if (!g_logger || !g_state.options.debug) {
        return;
    }
    /* Stack, not heap, and bounded: this can run on the post-crash path. Messages longer
     * than the buffer are truncated rather than allocated for. */
    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_logger(line);
}

void sentry_options_defaults(sentry_options_t *options)
{
    if (!options) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->environment = "production";
    /* Deliberately not unlimited. A capture API with no ceiling turns one looping bug into
     * an exhausted quota, and the first thing you lose when the quota is gone is crash
     * reports. See sentry_options_t for the arithmetic. */
    options->max_messages_per_minute = 10;
    options->message_repeat_window_ms = 10000;
#ifdef SENTRY_BUILD_ID_HEX
    /* Injected by scripts/release.sh, which passes the same value to the objcopy that
     * stamps the ELF — so the firmware's claim and the uploaded file always agree. */
    options->build_id = SENTRY_BUILD_ID_HEX;
#endif
#ifdef SENTRY_IMAGE_ADDR
    /* Read out of the linked ELF by the same script; see sentry_event_t::image_addr for
     * why a wrong value here fails silently rather than loudly. */
    options->image_addr = SENTRY_IMAGE_ADDR;
#endif
#ifdef SENTRY_IMAGE_SIZE
    options->image_size = SENTRY_IMAGE_SIZE;
#endif
#ifdef SENTRY_IMAGE_NAME
    options->image_name = SENTRY_IMAGE_NAME;
#else
    options->image_name = "firmware.elf";
#endif
}

void sentry_set_logger(sentry_logger_fn logger) { g_logger = logger; }

sentry_logger_fn sentry_get_logger(void) { return g_logger; }

bool sentry_init(const sentry_options_t *options)
{
    /* Reset everything except the logger, so that a failed init can still explain itself. */
    memset(&g_state, 0, sizeof(g_state));

    if (!options) {
        debug_log("no options passed; SDK disabled");
        return false;
    }
    g_state.options = *options;

    if (!options->dsn || options->dsn[0] == '\0') {
        debug_log("no DSN configured; SDK disabled");
        return false;
    }

    if (!sentry_dsn_parse(&g_state.dsn, options->dsn)) {
        debug_log("DSN failed to parse; SDK disabled");
        return false;
    }

    /* The URL and auth header never change after init, so build them once here rather than
     * on every send — the crash path should be doing as little as possible. */
    if (sentry_dsn_envelope_url(&g_state.dsn, g_state.envelope_url, sizeof(g_state.envelope_url))
            == 0
        || sentry_dsn_auth_header(&g_state.dsn, g_state.auth_header, sizeof(g_state.auth_header))
            == 0) {
        debug_log("DSN too long for the fixed buffers; SDK disabled");
        memset(&g_state.dsn, 0, sizeof(g_state.dsn));
        return false;
    }

    sentry_device_info_get(&g_state.device);
    /* Recovered before anything can raise an event, so a crash report built moments from
     * now still carries the trace the device died inside. Empty unless the last boot ended
     * in a panic while serving a request.
     *
     * Then the persisted slot is cleared, because nothing is in flight yet on this boot.
     * Leaving the old value there would mean a later, unrelated panic — on a boot where
     * nobody ever adopted a trace — recovers the previous crash's trace and claims the two
     * are the same incident. */
    sentry_device_trace_recover(&g_state.crash_trace, sizeof(g_state.crash_trace));
    sentry_trace_clear(&g_state.trace);
    sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
    sentry_throttle_init(
        &g_state.throttle, options->max_messages_per_minute, options->message_repeat_window_ms);
    g_state.enabled = true;

    debug_log("initialised %s, ingest %s", SENTRY_MICRO_SDK_USER_AGENT, g_state.envelope_url);
    debug_log("device %s %s, reset reason: %s", g_state.device.chip_model, g_state.device.device_id,
        sentry_reset_reason_name(g_state.device.reset_reason));

    if (sentry_reset_reason_is_crash(g_state.device.reset_reason)) {
        /* Where the crash event gets built and queued — see the roadmap in README.md. */
        debug_log("previous boot ended in a crash (%s)",
            sentry_reset_reason_name(g_state.device.reset_reason));
    }
    return true;
}

void sentry_close(void) { memset(&g_state, 0, sizeof(g_state)); }

bool sentry_is_enabled(void) { return g_state.enabled; }

const char *sentry_sdk_version(void) { return SENTRY_MICRO_SDK_VERSION; }

const sentry_dsn_t *sentry_get_dsn(void) { return &g_state.dsn; }

const char *sentry_get_envelope_url(void) { return g_state.envelope_url; }

const char *sentry_get_auth_header(void) { return g_state.auth_header; }

const char *sentry_get_org_id(void)
{
    return sentry_dsn_resolve_org_id(&g_state.dsn, g_state.options.org_id);
}

const sentry_device_info_t *sentry_get_device_info(void) { return &g_state.device; }

const sentry_options_t *sentry_get_options(void) { return &g_state.options; }

void sentry_set_transport(const sentry_transport_t *transport)
{
    g_state.transport = transport;
    debug_log("transport set to %s", sentry_transport_name(transport));
}

const sentry_transport_t *sentry_get_transport(void) { return g_state.transport; }

/** Deliver without touching the buffer. Shared by the direct path and by flush. */
static sentry_response_t deliver(const uint8_t *envelope, size_t len)
{
    sentry_headers_t headers;
    headers.auth = g_state.auth_header;
    headers.content_type = "application/x-sentry-envelope";

    sentry_response_t response
        = sentry_transport_send(g_state.transport, g_state.envelope_url, &headers, envelope, len);

    if (response.retry_after_ms > 0) {
        g_state.retry_after_uptime_ms = sentry_device_uptime_ms() + response.retry_after_ms;
        debug_log("backing off for %ums as instructed", (unsigned)response.retry_after_ms);
    }
    return response;
}

/** True when a previous response told us to wait and that wait has not elapsed. */
static bool in_backoff(void)
{
    return g_state.retry_after_uptime_ms > 0
        && sentry_device_uptime_ms() < g_state.retry_after_uptime_ms;
}

/**
 * Whether a failure is worth keeping the envelope for.
 *
 * REJECTED is excluded deliberately: ingest understood the request and will not accept it,
 * so buffering it means retrying something that can never succeed — forever, on every boot.
 */
static bool worth_retrying(sentry_send_result_t result)
{
    return result == SENTRY_SEND_UNAVAILABLE || result == SENTRY_SEND_ERROR
        || result == SENTRY_SEND_RATE_LIMITED;
}

sentry_response_t sentry_send_envelope(const uint8_t *envelope, size_t len)
{
    if (!g_state.enabled || !envelope || len == 0) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    sentry_response_t response;
    if (in_backoff()) {
        /* Do not even attempt it; go straight to the buffer if there is one. */
        response = sentry_response_make(SENTRY_SEND_RATE_LIMITED);
    } else {
        response = deliver(envelope, len);
        debug_log("sent %u bytes via %s: result %d, http %u", (unsigned)len,
            sentry_transport_name(g_state.transport), (int)response.result,
            (unsigned)response.http_status);
    }

    if (response.result != SENTRY_SEND_OK && g_state.buffering && worth_retrying(response.result)) {
        if (sentry_buffer_push(&g_state.buffer, envelope, len)) {
            debug_log(
                "buffered for retry (%u waiting)", (unsigned)sentry_buffer_count(&g_state.buffer));
        } else {
            debug_log("could not buffer the envelope; it is lost");
        }
    }
    return response;
}

bool sentry_trace_adopt(const char *sentry_trace, const char *baggage)
{
    if (!g_state.enabled) {
        return false;
    }
    uint8_t span_bytes[8];
    if (!sentry_device_random(span_bytes, sizeof(span_bytes))) {
        return false;
    }
    sentry_trace_context_t parsed;
    if (!sentry_trace_adopt_header(&parsed, sentry_trace, baggage, span_bytes)) {
        debug_log("ignored a malformed sentry-trace header");
        /* Parsing now goes into a local, not g_state.trace directly, so a bad header no
         * longer clears it as a side effect of the attempt. It still has to: leaving a
         * stale trace active here is exactly the "confidently wrong association" this
         * whole design exists to avoid — a later panic would attach to a request that was
         * rejected, not the one actually in flight. */
        sentry_trace_clear(&g_state.trace);
        sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
        return false;
    }

    if (!sentry_trace_can_continue(
            parsed.org_id, sentry_get_org_id(), g_state.options.strict_trace_continuation)) {
        /* Well-formed, but somebody else's organization. The request is still real and
         * still deserves a trace — the device just becomes its head instead of joining a
         * trace it has no business being part of. */
        uint8_t trace_bytes[16];
        if (!sentry_device_random(trace_bytes, sizeof(trace_bytes))) {
            sentry_trace_clear(&g_state.trace);
            sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
            return false;
        }
        sentry_trace_begin(&g_state.trace, trace_bytes, span_bytes, true);
        sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
        debug_log("refused a trace from a foreign org; began %s instead", g_state.trace.trace_id);
        return true;
    }

    g_state.trace = parsed;
    /* Persisted, not just held: the crash that matters most is the one that happens while
     * serving this request, and it is not reported until the next boot. */
    sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
    debug_log("joined trace %s%s", g_state.trace.trace_id,
        g_state.trace.replay_id[0] ? " (with a replay)" : "");
    return true;
}

bool sentry_trace_start(void)
{
    if (!g_state.enabled) {
        return false;
    }
    uint8_t trace_bytes[16];
    uint8_t span_bytes[8];
    if (!sentry_device_random(trace_bytes, sizeof(trace_bytes))
        || !sentry_device_random(span_bytes, sizeof(span_bytes))) {
        return false;
    }
    sentry_trace_begin(&g_state.trace, trace_bytes, span_bytes, true);
    sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
    debug_log("began trace %s", g_state.trace.trace_id);
    return true;
}

void sentry_trace_release(void)
{
    sentry_trace_clear(&g_state.trace);
    sentry_device_trace_persist(&g_state.trace, sizeof(g_state.trace));
}

const sentry_trace_context_t *sentry_trace_current(void) { return &g_state.trace; }

size_t sentry_trace_header(char *buf, size_t cap)
{
    return sentry_trace_header_write(buf, cap, &g_state.trace);
}

#if SENTRY_MICRO_METRICS_ENABLED
void sentry_metric_count(const char *name, int64_t delta, const char *unit)
{
    if (g_state.enabled && !sentry_metrics_count(&g_state.metrics, name, delta, unit)) {
        g_state.metrics_dropped++;
    }
}

void sentry_metric_gauge(const char *name, int64_t value, const char *unit)
{
    if (g_state.enabled && !sentry_metrics_gauge(&g_state.metrics, name, value, unit)) {
        g_state.metrics_dropped++;
    }
}

uint32_t sentry_metrics_dropped_count(void) { return g_state.metrics_dropped; }
#endif /* SENTRY_MICRO_METRICS_ENABLED */

#if SENTRY_MICRO_LOGS_ENABLED
void sentry_log(sentry_level_t level, const char *message, ...)
{
    if (!g_state.enabled || !message) {
        return;
    }
    char body[SENTRY_MICRO_LOG_BODY_LEN];
    va_list args;
    va_start(args, message);
    /* vsnprintf() returns how long the formatted result *would have been*, not how much fit
     * — the only place that information exists, since a caller downstream only ever sees
     * the already-truncated body and cannot recover it. */
    int written = vsnprintf(body, sizeof(body), message, args);
    va_end(args);
    bool truncated = written < 0 || (size_t)written >= sizeof(body);
    if (truncated && g_state.logs_truncated < UINT32_MAX) {
        g_state.logs_truncated++;
    }

    /* Recorded verbatim, or empty if nothing is active — resolved to a fallback only at
     * flush time. This is what lets a line written during a real operation stay attached to
     * it even if the operation has ended by the time the ring is serialised. */
    if (sentry_log_ring_push(&g_state.log_ring, level,
            g_state.trace.active ? g_state.trace.trace_id : "", sentry_device_uptime_us(), body,
            truncated)) {
        g_state.logs_dropped++;
    }
}

uint32_t sentry_logs_dropped_count(void) { return g_state.logs_dropped; }

/**
 * Lines recorded shorter than intended, since sentry_init().
 *
 * A device with no network still narrates this the way debug_log() would; once one does
 * reach Sentry, the same information rides along on the line itself as the `truncated`
 * attribute, so a fleet-wide answer does not require polling every device serially.
 */
uint32_t sentry_logs_truncated_count(void) { return g_state.logs_truncated; }

/**
 * Send whatever has accumulated in the log ring, if anything has.
 *
 * Called from sentry_flush() only — never from sentry_log() itself, for the same reason
 * flush_metrics() (below) is never called from the recording calls: recording touches a
 * ring, and only a flush touches the transport.
 *
 * Unlike flush_metrics(), this never mints a trace by calling sentry_trace_start(): doing
 * so mutates g_state.trace and never releases it again, which is harmless the way metrics
 * uses it but would be wrong here — the fallback below is scoped to this one envelope only.
 */
static void flush_logs(void)
{
    if (sentry_log_ring_empty(&g_state.log_ring)) {
        return;
    }

    /* Every log needs a timestamp, so a device that has not been told the date cannot send
     * yet. Held rather than dropped, same reasoning as metrics: a longer covered interval is
     * still true, unlike a line timestamped to the epoch. */
    uint64_t now_unix_us = sentry_device_unix_time_us();
    if (now_unix_us == 0) {
        debug_log("holding logs: the device has not been told the date");
        return;
    }

    /* A fallback trace_id for any entry that had none active when it was recorded — minted
     * locally for this flush only and never written to g_state.trace, so it cannot outlive
     * the envelope it was minted for. */
    char fallback_trace_id[SENTRY_MICRO_TRACE_ID_LEN] = { 0 };
    uint8_t trace_bytes[16];
    if (sentry_device_random(trace_bytes, sizeof(trace_bytes))) {
        sentry_trace_id_format(fallback_trace_id, trace_bytes);
    }
    if (!fallback_trace_id[0]) {
        /* No entropy, so nothing serialises at all: sentry_log_envelope_write() refuses a
         * batch with no fallback whether or not any entry actually needs one. Held rather
         * than dropped, for the same reason the clock above is — the RNG answering is a
         * change of state this exact batch survives, unlike the size failure below. */
        debug_log("holding logs: no trace id could be minted for the batch");
        return;
    }

    char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    size_t needed = sentry_log_envelope_write(envelope, sizeof(envelope), &g_state.log_ring,
        fallback_trace_id, g_state.device.device_id, sentry_device_uptime_us(), now_unix_us);
    if (needed == 0 || needed >= sizeof(envelope)) {
        /* Too large to encode, and it will be exactly as large on every later flush: what
         * pushes a ring over the budget is JSON escaping — a `"` or `\` doubles, a control
         * character becomes six bytes — and holding the batch does not shrink it. Keeping it
         * would wedge the ring behind one unsendable batch forever, and every line recorded
         * after it too, so drop it and keep going. That is the same trade the buffered
         * envelope loop in sentry_flush() already makes for an entry it cannot read, and
         * these lines are counted the way an eviction counts them, because they are gone
         * for the same reason: the ring had no room to carry them any further. */
        uint32_t lost = g_state.log_ring.count;
        debug_log("dropping %u logs: need %u bytes, envelope buffer is %u", (unsigned)lost,
            (unsigned)needed, (unsigned)sizeof(envelope));
        g_state.logs_dropped
            = UINT32_MAX - g_state.logs_dropped < lost ? UINT32_MAX : g_state.logs_dropped + lost;
        sentry_log_ring_reset(&g_state.log_ring);
        return;
    }

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
    /* Cleared whether or not the send succeeded, same reasoning as flush_metrics(): a failed
     * send has already been buffered for retry by sentry_send_envelope() if it was worth
     * retrying, so keeping the ring too would resend every line on the next flush. */
    sentry_log_ring_reset(&g_state.log_ring);
    debug_log("flushed logs: result %d", (int)response.result);
}
#endif /* SENTRY_MICRO_LOGS_ENABLED */

/**
 * Send whatever has accumulated, if anything has.
 *
 * Called from sentry_flush() rather than from the recording calls, which is the property
 * that makes a counter safe in a render loop: recording touches a table, and only the
 * flush — already on an interval the firmware chose — touches the transport.
 */
#if SENTRY_MICRO_METRICS_ENABLED
static void flush_metrics(void)
{
    if (sentry_metrics_empty(&g_state.metrics)) {
        return;
    }
    /* Every metric needs a trace id from the propagation context. Idle is the normal state
     * for a device reporting heap, so one is minted for the batch when nothing is in
     * flight: the numbers correlate with each other and claim nothing about a user action
     * they had no part in. */
    if (!g_state.trace.active && !sentry_trace_start()) {
        return;
    }

    /* Every metric needs a timestamp, so a device that has not been told the date cannot
     * send yet. Retained rather than dropped, unlike a transaction: a counter that keeps
     * accumulating simply covers a longer interval, whereas a duration would go stale. */
    uint64_t now_unix_us = sentry_device_unix_time_us();
    if (now_unix_us == 0) {
        debug_log("holding metrics: the device has not been told the date");
        return;
    }

    char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    size_t needed = sentry_metrics_envelope_write(
        envelope, sizeof(envelope), &g_state.metrics, g_state.trace.trace_id, now_unix_us);
    if (needed == 0 || needed >= sizeof(envelope)) {
        debug_log("metrics need %u bytes, envelope buffer is %u", (unsigned)needed,
            (unsigned)sizeof(envelope));
        return;
    }

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
    /* Cleared whether or not the send succeeded, because sentry_send_envelope() has already
     * buffered it if it was worth retrying. Keeping the table as well would double-count
     * every value on the next flush. */
    sentry_metrics_reset(&g_state.metrics);
    debug_log("flushed metrics: result %d", (int)response.result);
}
#endif /* SENTRY_MICRO_METRICS_ENABLED */

bool sentry_transaction_start(sentry_transaction_t *txn, const char *name, const char *op)
{
    if (!g_state.enabled || !txn) {
        return false;
    }
    /* An operation still needs a trace when nobody handed us one — it is a real unit of
     * work either way, and this device becomes its head. */
    if (!g_state.trace.active && !sentry_trace_start()) {
        return false;
    }
    /* An explicit "no" upstream is honoured rather than overridden. A deferred decision
     * (no third field in the header) is not a no. */
    if (g_state.trace.has_sampling_decision && !g_state.trace.sampled) {
        return false;
    }

    sentry_transaction_begin(txn, &g_state.trace, name, op, sentry_device_uptime_us());
    debug_log(
        "transaction %s began on trace %s", name ? name : "operation", g_state.trace.trace_id);
    return true;
}

sentry_span_t *sentry_transaction_start_child(
    sentry_transaction_t *txn, const char *op, const char *description)
{
    if (!g_state.enabled || !txn) {
        return NULL;
    }
    uint8_t span_bytes[8];
    if (!sentry_device_random(span_bytes, sizeof(span_bytes))) {
        return NULL;
    }
    return sentry_span_open(txn, op, description, span_bytes, sentry_device_uptime_us());
}

void sentry_span_finish(sentry_span_t *span) { sentry_span_close(span, sentry_device_uptime_us()); }

void sentry_span_set_attribute(sentry_span_t *span, const char *key, int64_t value)
{
    sentry_span_set_number(span, key, value);
}

sentry_response_t sentry_transaction_finish(sentry_transaction_t *txn)
{
    if (!g_state.enabled || !txn || !txn->active) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    if (!sentry_transaction_end_at(txn, sentry_device_uptime_us(), sentry_device_unix_time_us())) {
        /* No date, so no honest place on the timeline. Dropped rather than anchored to the
         * epoch, and said out loud because the alternative is a trace nobody can explain.
         * Telling the device the time is the application's job — see
         * sentry_device_unix_time_us(). */
        debug_log("dropped a transaction: the device has not been told the date");
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    char event_id[SENTRY_MICRO_EVENT_ID_LEN];
    uint8_t random_bytes[16];
    if (!sentry_device_random(random_bytes, sizeof(random_bytes))) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }
    sentry_event_id_format(event_id, random_bytes);

    sentry_transaction_meta_t meta;
    meta.event_id = event_id;
    meta.release = g_state.options.release;
    meta.environment = g_state.options.environment;
    meta.board = g_state.options.board;
    meta.device = &g_state.device;

    char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    size_t needed = sentry_transaction_envelope_write(envelope, sizeof(envelope), txn, &meta);
    if (needed == 0 || needed >= sizeof(envelope)) {
        /* Raise SENTRY_MICRO_ENVELOPE_BUFFER_BYTES or lower SENTRY_MICRO_MAX_SPANS;
         * truncating would produce a trace that looks complete and is not. */
        debug_log("transaction needs %u bytes, envelope buffer is %u", (unsigned)needed,
            (unsigned)sizeof(envelope));
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    if (txn->dropped_spans > 0) {
        debug_log("transaction dropped %u span(s); raise SENTRY_MICRO_MAX_SPANS",
            (unsigned)txn->dropped_spans);
    }
    return sentry_send_envelope((const uint8_t *)envelope, needed);
}

sentry_response_t sentry_capture_message(sentry_level_t level, const char *message)
{
    if (!g_state.enabled) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    /* Before anything is built, not after: the point of the throttle is to make a flood
     * cheap, and formatting an event we are about to discard is most of the cost. */
    if (!sentry_throttle_allow(&g_state.throttle, level, message, sentry_device_uptime_ms())) {
        debug_log("throttled: \"%s\" (%u suppressed so far)", message ? message : "",
            (unsigned)sentry_throttle_suppressed(&g_state.throttle));
        return sentry_response_make(SENTRY_SEND_RATE_LIMITED);
    }

    char event_id[SENTRY_MICRO_EVENT_ID_LEN];
    sentry_event_t event;
    if (!sentry_event_prepare(&event, event_id)) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }
    event.level = level;
    event.message = message;
    debug_log(
        "capturing %s as %s: \"%s\"", event_id, sentry_level_name(level), message ? message : "");

    /* Stack, matching sentry_flush(). The no-allocation rule is not about this call being
     * on the crash path — it is that a device with a fragmented heap must still be able to
     * report why. */
    char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
    if (needed == 0 || needed >= sizeof(envelope)) {
        /* REJECTED rather than ERROR so the caller does not retry and the core does not
         * buffer: the same message would not fit next time either. Raise
         * SENTRY_MICRO_ENVELOPE_BUFFER_BYTES if this is your message rather than a bug. */
        debug_log("message needs %u bytes, envelope buffer is %u", (unsigned)needed,
            (unsigned)sizeof(envelope));
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);

    /* Restart the repeat window from here, not from where this call began. A send with no
     * route blocks for the transport's timeout, which is longer than the window itself —
     * so measuring from the start means the window is always already over by the time the
     * caller loops round, and identical messages are never suppressed. */
    sentry_throttle_settle(&g_state.throttle, sentry_device_uptime_ms());
    return response;
}

uint32_t sentry_suppressed_count(void) { return sentry_throttle_suppressed(&g_state.throttle); }

bool sentry_enable_buffering(const sentry_storage_t *storage)
{
    g_state.buffering = sentry_buffer_init(&g_state.buffer, storage);
    if (g_state.buffering) {
        debug_log("buffering enabled: %u waiting, %u previously dropped",
            (unsigned)sentry_buffer_count(&g_state.buffer),
            (unsigned)sentry_buffer_dropped(&g_state.buffer));
    } else {
        debug_log("storage unusable; continuing without buffering");
    }
    return g_state.buffering;
}

uint32_t sentry_buffered_count(void)
{
    return g_state.buffering ? sentry_buffer_count(&g_state.buffer) : 0;
}

uint32_t sentry_dropped_count(void)
{
    return g_state.buffering ? sentry_buffer_dropped(&g_state.buffer) : 0;
}

uint32_t sentry_flush(uint32_t max_events)
{
    if (!g_state.enabled || in_backoff()) {
        return 0;
    }
    /* Before the buffered envelopes, and outside the buffering check: metrics and logs
     * accumulate whether or not offline buffering was ever enabled, and a device that never
     * buffers still wants its heap reported and its console mirrored. Either call compiles
     * away entirely when its feature is built out — see SENTRY_MICRO_METRICS_ENABLED /
     * SENTRY_MICRO_LOGS_ENABLED. */
#if SENTRY_MICRO_METRICS_ENABLED
    flush_metrics();
#endif
#if SENTRY_MICRO_LOGS_ENABLED
    flush_logs();
#endif

    if (!g_state.buffering) {
        return 0;
    }

    uint8_t envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    uint32_t delivered = 0;

    while (delivered < max_events && sentry_buffer_count(&g_state.buffer) > 0) {
        size_t len = 0;
        if (!sentry_buffer_peek(&g_state.buffer, envelope, sizeof(envelope), &len)) {
            /* Unreadable or too large for the scratch buffer. Retrying it would wedge the
             * queue behind one bad entry forever, so drop it and keep going. */
            debug_log("discarding an unreadable buffered envelope");
            sentry_buffer_pop(&g_state.buffer);
            continue;
        }

        sentry_response_t response = deliver(envelope, len);
        if (response.result == SENTRY_SEND_OK) {
            sentry_buffer_pop(&g_state.buffer);
            delivered++;
            continue;
        }

        if (!worth_retrying(response.result)) {
            /* Permanently rejected: dropping it is the only way the queue ever drains. */
            debug_log("dropping a rejected envelope (http %u)", (unsigned)response.http_status);
            sentry_buffer_pop(&g_state.buffer);
            continue;
        }

        /* Still no route. Stop rather than retrying every queued event against a network
         * that is plainly down — one failed attempt per flush, not N. */
        break;
    }

    if (delivered > 0) {
        debug_log("flushed %u envelopes, %u still waiting", (unsigned)delivered,
            (unsigned)sentry_buffer_count(&g_state.buffer));
    }
    return delivered;
}

bool sentry_event_attach_coredump(sentry_event_t *event, sentry_coredump_t *storage)
{
    if (!event || !storage || !g_state.enabled) {
        return false;
    }
    if (!sentry_coredump_read(storage) || !storage->available) {
        return false;
    }

    event->coredump = storage;
    event->level = SENTRY_LEVEL_FATAL;
    /* The crash belongs to the trace it died inside, never to whatever is being served
     * now — overriding what sentry_event_prepare() set. NULL when the previous boot was
     * not serving anything, which is the honest answer rather than a gap. */
    event->trace = g_state.crash_trace.active ? &g_state.crash_trace : NULL;

    debug_log("crash recovered: %s in %s, %u frames%s", storage->exception_type, storage->task_name,
        (unsigned)storage->frame_count, storage->truncated ? " (truncated)" : "");
    return true;
}

bool sentry_event_prepare(sentry_event_t *event, char *event_id_buf)
{
    if (!event || !event_id_buf) {
        return false;
    }
    memset(event, 0, sizeof(*event));

    if (!g_state.enabled) {
        return false;
    }

    uint8_t random_bytes[16];
    if (!sentry_device_random(random_bytes, sizeof(random_bytes))) {
        /* Without a unique id a boot loop would report the same event over and over,
         * be deduplicated into one issue, and hide the very frequency that matters. */
        debug_log("no entropy for an event id; dropping event");
        return false;
    }
    sentry_event_id_format(event_id_buf, random_bytes);

    event->event_id = event_id_buf;
    event->level = SENTRY_LEVEL_ERROR;
    event->release = g_state.options.release;
    event->environment = g_state.options.environment;
    event->board = g_state.options.board;
    event->device = &g_state.device;
    event->build_id = g_state.options.build_id;
    event->image_addr = g_state.options.image_addr;
    event->image_size = g_state.options.image_size;
    event->image_name = g_state.options.image_name;
    event->trace = g_state.trace.active ? &g_state.trace : NULL;

    /* Sampled now rather than reused from init, so the numbers describe the moment the
     * event happened — which for a heap leak is the entire point. */
    event->timestamp = sentry_device_unix_time();
    event->free_heap_bytes = sentry_device_free_heap();
    event->min_free_heap_bytes = sentry_device_min_free_heap();
    event->uptime_ms = sentry_device_uptime_ms();
    return true;
}
