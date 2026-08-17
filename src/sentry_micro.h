/**
 * sentry-micro — the Sentry SDK for microcontrollers.
 *
 * This is the only header you need to include. It gives you the C API below, and — when
 * compiled as C++ — an ergonomic wrapper in `namespace sentry` (see `sentry_micro.hpp`).
 *
 * C:
 *
 *     sentry_options_t options;
 *     sentry_options_defaults(&options);
 *     options.dsn = "https://examplePublicKey@o0.ingest.sentry.io/0";
 *     options.release = "my-firmware@1.0.0";
 *     sentry_init(&options);
 *
 * C++ / Arduino:
 *
 *     sentry::Options options;
 *     options.dsn = "https://examplePublicKey@o0.ingest.sentry.io/0";
 *     options.release = "my-firmware@1.0.0";
 *     sentry::init(options);
 *
 * The C API is the real one; the C++ names are inline wrappers over it with no separate
 * state. That keeps the SDK usable from a plain ESP-IDF component with no C++ runtime,
 * while Arduino users get the shorter spelling.
 *
 * Unlike `sentry-native`, this SDK assumes nothing about the platform beyond "there is a C
 * compiler and some way to move bytes": no virtual memory, no filesystem, no threads, no
 * allocation on the reporting path. See README.md for how that shapes the API.
 *
 * Licensed under the MIT license. https://github.com/getsentry/sentry-micro
 */
#ifndef SENTRY_MICRO_H_INCLUDED
#define SENTRY_MICRO_H_INCLUDED

#include "core/sentry_boot.h"
#include "core/sentry_dsn.h"
#include "core/sentry_envelope.h"
#include "core/sentry_transport.h"
#include "device/sentry_device.h"

/** Version of this SDK, as a string, for compile-time checks. */
#define SENTRY_MICRO_VERSION SENTRY_MICRO_SDK_VERSION

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK configuration, passed once to `sentry_init()`.
 *
 * All `const char *` fields are stored by pointer, not copied — pass string literals or
 * something with static lifetime. This is deliberate: copying a handful of strings would
 * cost heap on a chip that may have 40 KB free, and in practice every one of these is a
 * compile-time constant or a build-flag define.
 *
 * Always run `sentry_options_defaults()` over this before setting fields. C has no default
 * member initialisers, and an uninitialised `environment` is a dangling pointer.
 */
typedef struct {
    /**
     * The DSN from your Sentry project settings. Required; if it does not parse, the SDK
     * stays disabled and every other call becomes a no-op.
     */
    const char *dsn;

    /**
     * Release identifier, conventionally `package@version` — e.g. `"chromabay@1.4.2"`.
     * This is what ties an event to the uploaded debug files, so **symbolication does not
     * work without it matching what `sentry-cli` uploaded**.
     */
    const char *release;

    /** `"production"`, `"development"`, … Groups and filters events in the UI. */
    const char *environment;

    /** Free-form board/hardware identifier attached as a tag, e.g. `"m5-atoms3"`. */
    const char *board;

    /**
     * Organization id, for trace propagation and the Dynamic Sampling Context.
     *
     * Normally left unset: it is recovered automatically from an `o<digits>.` DSN host.
     * Set it when your ingest host carries no such prefix — self-hosted Sentry, or a
     * custom ingest domain — where it cannot be inferred. Takes precedence over the DSN.
     */
    const char *org_id;

    /** When true, the SDK narrates what it is doing through the logger. Off in production. */
    bool debug;
} sentry_options_t;

/** Fill `options` with defaults. Call this first; then override what you need. */
void sentry_options_defaults(sentry_options_t *options);

/**
 * Receives the SDK's debug output, already formatted and NUL-terminated, without a newline.
 *
 * A hook rather than a hard-coded `printf` because there is no portable console here: on a
 * native-USB ESP32 the Arduino `Serial` object and C `printf` go to *different* physical
 * interfaces, so a built-in choice would silently log to a pin nobody is watching. The C++
 * layer installs an Arduino `Serial` logger for you; C users install their own.
 */
typedef void (*sentry_logger_fn)(const char *message);

/** Route SDK debug output to `logger`, or pass NULL to discard it. */
void sentry_set_logger(sentry_logger_fn logger);

/** The installed logger, or NULL. Lets a layer install a default only if none is set. */
sentry_logger_fn sentry_get_logger(void);

/**
 * Initialise the SDK.
 *
 * Call once at startup, as early as possible — the reset reason from the *previous* boot is
 * read here, so anything that reboots before `sentry_init()` runs is invisible.
 *
 * Returns false if the DSN is missing or unparseable, in which case the SDK is disabled.
 * Callers are expected to ignore that and carry on: crash reporting must never be
 * load-bearing for the firmware it is reporting on.
 */
bool sentry_init(const sentry_options_t *options);

/** Tear down and disable the SDK. Rarely needed on a device that only ever reboots. */
void sentry_close(void);

/** True when `sentry_init()` succeeded and the SDK will do work. */
bool sentry_is_enabled(void);

/** SDK version string, e.g. `"0.1.0"`. Always available, even before init. */
const char *sentry_sdk_version(void);

/**
 * The parsed DSN. Never NULL; its `valid` field is false before a successful init.
 * Transports use `->host` to whitelist where they are willing to POST.
 */
const sentry_dsn_t *sentry_get_dsn(void);

/** Fully-formed envelope endpoint, e.g. `https://host/api/42/envelope/`. "" if disabled. */
const char *sentry_get_envelope_url(void);

/** Value for the `X-Sentry-Auth` header. "" if disabled. */
const char *sentry_get_auth_header(void);

/**
 * Effective organization id — the `org_id` option if set, else the one recovered from the
 * DSN host, else "". Never NULL.
 */
const char *sentry_get_org_id(void);

/** This boot's device facts. Never NULL; zeroed before a successful init. */
const sentry_device_info_t *sentry_get_device_info(void);

/** The configuration `sentry_init()` was called with. Never NULL. */
const sentry_options_t *sentry_get_options(void);

/**
 * Register the transport used to deliver envelopes.
 *
 * Stored by pointer and not copied, so it must outlive the SDK — a file-scope or static
 * object, not a stack local. Pass NULL to detach, after which delivery reports
 * `SENTRY_SEND_UNAVAILABLE` and the core buffers instead.
 */
void sentry_set_transport(const sentry_transport_t *transport);

/** The registered transport, or NULL. */
const sentry_transport_t *sentry_get_transport(void);

/**
 * Fill `event` with everything the SDK already knows: a fresh event id, the configured
 * release/environment/board, this boot's device facts, and live heap and uptime sampled
 * now. The caller then sets `level` and `message` and writes it with
 * `sentry_envelope_write()`.
 *
 * `event_id_buf` must be at least `SENTRY_MICRO_EVENT_ID_LEN` bytes and must outlive the
 * event, which points into it rather than copying — the same no-allocation rule as
 * everywhere else on this path.
 *
 * Returns false when the SDK is disabled or no entropy is available for the id.
 */
bool sentry_event_prepare(sentry_event_t *event, char *event_id_buf);

/**
 * Deliver a fully-built envelope through the registered transport.
 *
 * Fills in the ingest URL and the auth header from SDK state, so callers never assemble
 * either by hand — the transport is handed exactly what it needs and nothing it could get
 * wrong. Returns `SENTRY_SEND_UNAVAILABLE` when the SDK is disabled or no transport is
 * attached, which is the same "buffer it and retry" signal a downed radio produces.
 */
sentry_response_t sentry_send_envelope(const uint8_t *envelope, size_t len);

#ifdef __cplusplus
} /* extern "C" */

#    include "sentry_micro.hpp"
#endif

#endif /* SENTRY_MICRO_H_INCLUDED */
