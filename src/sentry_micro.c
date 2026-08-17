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
