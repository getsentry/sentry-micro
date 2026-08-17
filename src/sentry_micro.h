/**
 * sentry-micro — the Sentry SDK for microcontrollers.
 *
 * This is the only header you need to include:
 *
 *     #include <sentry_micro.h>
 *
 *     sentry::Options opts;
 *     opts.dsn = "https://examplePublicKey@o0.ingest.sentry.io/0";
 *     opts.release = "my-firmware@1.0.0";
 *     sentry::init(opts);
 *
 * Unlike `sentry-native`, this SDK assumes nothing about the platform beyond "there is a
 * C compiler and some way to move bytes": no virtual memory, no filesystem, no threads,
 * no allocation on the reporting path. See README.md for how that shapes the API.
 *
 * Licensed under the MIT license. https://github.com/getsentry/sentry-micro
 */
#ifndef SENTRY_MICRO_H_INCLUDED
#define SENTRY_MICRO_H_INCLUDED

#include "core/sentry_boot.h"
#include "core/sentry_dsn.h"
#include "device/sentry_device.h"
#include "transport/sentry_transport.h"

/** Version of this SDK, as a string, for compile-time checks. */
#define SENTRY_MICRO_VERSION SENTRY_MICRO_SDK_VERSION

namespace sentry {

/**
 * SDK configuration, passed once to `init()`.
 *
 * All `const char *` fields are stored by pointer, not copied — pass string literals or
 * something with static lifetime. This is deliberate: copying a handful of strings would
 * cost heap on a chip that may have 40 KB free, and in practice every one of these is a
 * compile-time constant or a build-flag define.
 */
struct Options {
    /**
     * The DSN from your Sentry project settings. Required; if it does not parse the SDK
     * stays disabled and every other call becomes a no-op.
     */
    const char *dsn = nullptr;

    /**
     * Release identifier, conventionally `package@version` — e.g. `"chromabay@1.4.2"`.
     * This is what ties an event to the uploaded debug files, so **symbolication does
     * not work without it matching what `sentry-cli` uploaded**.
     */
    const char *release = nullptr;

    /** `"production"`, `"development"`, … Groups and filters events in the UI. */
    const char *environment = "production";

    /** Free-form board/hardware identifier attached as a tag, e.g. `"m5-atoms3"`. */
    const char *board = nullptr;

    /** When true, the SDK narrates what it is doing to `Serial`. Off in production. */
    bool debug = false;
};

/**
 * Initialise the SDK.
 *
 * Call once from `setup()`, as early as possible — the reset reason from the *previous*
 * boot is read here, so anything that reboots before `init()` runs is invisible.
 *
 * Returns false if the DSN is missing or unparseable, in which case the SDK is disabled.
 */
bool init(const Options &options);

/** Tear down and disable the SDK. Rarely needed on a device that only ever reboots. */
void shutdown();

/** True when `init()` succeeded and the SDK will do work. */
bool is_enabled();

/** SDK version string, e.g. `"0.1.0"`. Always available, even before `init()`. */
const char *sdk_version();

/**
 * The parsed DSN. `valid` is false before a successful `init()`.
 * Transports use `dsn().host` to whitelist where they are willing to POST.
 */
const sentry_dsn_t &dsn();

/** Fully-formed envelope endpoint, e.g. `https://host/api/42/envelope/`. "" if disabled. */
const char *envelope_url();

/** Value for the `X-Sentry-Auth` header. "" if disabled. */
const char *auth_header();

/** This boot's device facts. Populated by `init()`; zeroed before that. */
const sentry_device_info_t &device_info();

/** The configuration `init()` was called with. */
const Options &options();

} // namespace sentry

#endif /* SENTRY_MICRO_H_INCLUDED */
