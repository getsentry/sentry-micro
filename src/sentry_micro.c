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
    if (!g_state.enabled || !g_state.buffering || in_backoff()) {
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

    /* Sampled now rather than reused from init, so the numbers describe the moment the
     * event happened — which for a heap leak is the entire point. */
    event->timestamp = sentry_device_unix_time();
    event->free_heap_bytes = sentry_device_free_heap();
    event->min_free_heap_bytes = sentry_device_min_free_heap();
    event->uptime_ms = sentry_device_uptime_ms();
    return true;
}
