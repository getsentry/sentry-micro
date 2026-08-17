#include "sentry_micro.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

namespace sentry {
namespace {

/*
 * One statically-allocated instance of SDK state.
 *
 * A singleton is not laziness here — it is the point. There is exactly one device, one
 * DSN, and one crash to report, and the crash handler must be able to reach this state
 * without dereferencing anything that could have been corrupted or freed. Static storage
 * means the SDK costs a fixed, knowable number of bytes of RAM that shows up in the map
 * file, which matters when the whole budget is 320 KB.
 */
struct State {
    Options options;
    sentry_dsn_t dsn;
    sentry_device_info_t device;
    char envelope_url[SENTRY_MICRO_MAX_URL_LEN];
    char auth_header[SENTRY_MICRO_MAX_AUTH_LEN];
    bool enabled;
};

State g_state = {};

void debug_log(const char *fmt, ...)
{
#if defined(ARDUINO)
    if (!g_state.options.debug) {
        return;
    }
    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Serial.print("[sentry] ");
    Serial.println(line);
#else
    (void)fmt;
#endif
}

} // namespace

bool init(const Options &options)
{
    shutdown();
    g_state.options = options;

    if (!options.dsn || options.dsn[0] == '\0') {
        debug_log("no DSN configured; SDK disabled");
        return false;
    }

    if (!sentry_dsn_parse(&g_state.dsn, options.dsn)) {
        debug_log("DSN failed to parse; SDK disabled");
        return false;
    }

    /* The URL and auth header never change after init, so build them once here rather
     * than on every send — the crash path should be doing as little as possible. */
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

void shutdown() { g_state = State {}; }

bool is_enabled() { return g_state.enabled; }

const char *sdk_version() { return SENTRY_MICRO_SDK_VERSION; }

const sentry_dsn_t &dsn() { return g_state.dsn; }

const char *envelope_url() { return g_state.envelope_url; }

const char *auth_header() { return g_state.auth_header; }

const sentry_device_info_t &device_info() { return g_state.device; }

const Options &options() { return g_state.options; }

} // namespace sentry
