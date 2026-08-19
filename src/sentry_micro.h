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
#include "core/sentry_buffer.h"
#include "core/sentry_dsn.h"
#include "core/sentry_envelope.h"
#include "core/sentry_metrics.h"
#include "core/sentry_span.h"
#include "core/sentry_throttle.h"
#include "core/sentry_trace.h"
#include "core/sentry_transport.h"
#include "device/sentry_coredump_device.h"
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
     *
     * Metadata, not a symbolication key. It groups events, drives release health, and is
     * what suspect commits hang off — but a backtrace resolves whether or not it is set,
     * and whatever it is set to. Symbolication matches `debug_id`, derived from the GNU
     * build-id, against the uploaded ELF; `sentry-cli debug-files upload` takes no release
     * argument at all.
     *
     * Said explicitly because the opposite is easy to assume, and this header asserted it
     * for a while: JavaScript source maps *are* release-scoped, so the rule carries over
     * from web SDKs where it does not hold. Releases here can be renamed without breaking
     * a single stack trace.
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
     *
     * This is also what `sentry_trace_adopt()` compares an incoming `sentry-org_id` against
     * before joining a trace — see `strict_trace_continuation` below for what happens when
     * they disagree.
     */
    const char *org_id;

    /**
     * Whether `sentry_trace_adopt()` requires *both* sides to know their org id and agree,
     * rather than giving the benefit of the doubt when only one side does.
     *
     * Off by default: most callers of a device have no idea what org id means, and
     * treating their silence as a mismatch would refuse to join traces from perfectly
     * ordinary requests. Turn this on only where an unknown org id on either side should
     * itself be treated as suspicious — a closed fleet talking to a single known org, for
     * instance. A trace with a *known, different* org id is always refused either way; this
     * only governs the ambiguous case where one side has nothing to compare.
     */
    bool strict_trace_continuation;

    /**
     * GNU build-id of this firmware, lowercase hex — the key Sentry uses to find the debug
     * files uploaded for this exact build. Optional; without it, addresses in a backtrace
     * can never be resolved to functions and lines.
     *
     * Normally left unset: `scripts/release.sh` chooses the id, injects it as
     * `SENTRY_BUILD_ID_HEX`, and `sentry_options_defaults()` picks it up automatically, so
     * the firmware and the uploaded ELF cannot disagree.
     */
    const char *build_id;

    /**
     * Where the firmware image is loaded and how far it extends.
     *
     * Both must match the ELF that was uploaded, or Sentry cannot map an address back to a
     * function. Normally left unset: `scripts/release.sh` reads them out of the linked ELF
     * and injects them, the same way it does the build-id.
     */
    uint64_t image_addr;
    uint64_t image_size;

    /** Firmware image name, shown as the module beside each frame. Defaults to the env. */
    const char *image_name;

    /**
     * Ceiling on `sentry_capture_message()` calls that become events, per minute.
     * 0 removes the limit.
     *
     * This exists because the API is easy to call from a loop and a loop runs forever. At
     * the default of 10 a single device can bill 14,400 events a day; a fleet of a hundred
     * misbehaving ones can exhaust a quota before anybody notices, and the events that get
     * dropped once you are over quota are the crash reports. Lower it for a large fleet.
     *
     * Never applies to crash reports, which are sent by a different path — a throttle that
     * could eat the panic you rebooted from would be worse than no throttle.
     */
    uint16_t max_messages_per_minute;

    /**
     * How long an identical message stays suppressed, in milliseconds. 0 disables it.
     *
     * The common failure is not many different messages, it is one message from a loop: a
     * sensor read that starts failing at 50 Hz. Within this window that becomes one event
     * rather than thousands, and — unlike the per-minute ceiling — it does not crowd out
     * whatever *else* the firmware has to say.
     */
    uint32_t message_repeat_window_ms;

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
 * Adopt the trace context arriving with a request, so events raised while serving it are
 * joined to whatever called.
 *
 *     sentry_trace_adopt(sentry_trace_header, baggage_header);
 *     handle_the_request();
 *     sentry_trace_release();
 *
 * `sentry_trace` is the `sentry-trace` header value; `baggage` may be NULL. Both are read
 * during the call and never retained. Returns false — leaving no trace active — if the
 * header is malformed; see `core/sentry_trace.h` for why a half-readable header is
 * rejected rather than partly believed.
 *
 * A well-formed header carrying a `sentry-org_id` that disagrees with this device's own
 * (see `options.org_id`) is not adopted either — joining it would put this device's
 * telemetry in someone else's organization. The request still gets a trace; the device
 * just becomes its head instead, exactly as `sentry_trace_start()` would. Returns true in
 * that case too, since a trace is active either way — check `sentry_trace_current()` if
 * the caller needs to know which one it got.
 *
 * The SDK does not care how those strings reached the device. BLE characteristic, HTTP
 * header, a field in your own protocol: they are two strings by the time they get here.
 *
 * **Release it when the operation finishes.** A device that holds the last trace it saw
 * will attach an unrelated panic hours later to that interaction, which reads as a real
 * causal link and is not one.
 *
 * Adopting does **not** disturb a crash recovered from the previous boot. The app that
 * comes to collect a crash report typically connects and offers a new trace in the same
 * breath, so the two are deliberately kept apart: `sentry_event_attach_coredump()` uses the
 * trace the device died inside, whatever is being served now. Reporting the last boot
 * before or after adopting therefore gives the same answer.
 *
 * One thing worth knowing if a single trace covers a long-lived connection rather than one
 * command: a Sentry replay ends after 60 minutes, or 15 minutes without a click or
 * navigation. A `replay_id` held past that still links — replays are stored — but it names
 * the session that *was* recording, not the interaction at hand. Re-adopting with a fresh
 * pair when the app's replay rotates keeps the link precise.
 */
bool sentry_trace_adopt(const char *sentry_trace, const char *baggage);

/**
 * Begin a trace this device is the origin of — a boot, an OTA, a scheduled task.
 *
 * Returns false when the SDK is disabled or there is no entropy for the ids.
 */
bool sentry_trace_start(void);

/** End the active trace. Safe to call when none is active. */
void sentry_trace_release(void);

/** The active trace, or NULL. Its `active` field is false when nothing is in flight. */
const sentry_trace_context_t *sentry_trace_current(void);

/**
 * Write a `sentry-trace` header value for a call this device is making outward.
 *
 * Needs 52 bytes. Returns the length, or 0 when no trace is active — in which case send no
 * header at all rather than an empty one, so the far end starts its own trace instead of
 * joining a nonexistent one.
 */
size_t sentry_trace_header(char *buf, size_t cap);

/**
 * Begin a transaction for an operation the device is about to perform.
 *
 *     sentry_transaction_t txn;
 *     sentry_transaction_start(&txn, "set-colour", "device.operation");
 *     sentry_span_t *decode = sentry_transaction_start_child(&txn, "ble.decode", NULL);
 *     ...
 *     sentry_span_finish(decode);
 *     sentry_transaction_finish(&txn);
 *
 * The verbs match sentry-native — `_start` and `_finish`, with child spans started from
 * their parent — so anyone who has used the desktop C SDK already knows the shape. What
 * deliberately does *not* match is ownership: sentry-native hands back heap-managed
 * `sentry_value_t` handles, and this SDK does not allocate.
 *
 * **You own `txn`** — a local in the function running the operation, like the
 * `sentry_coredump_t` a crash report is read into. It is a few hundred bytes and costs
 * nothing when no operation is running, which is most of the time. The SDK deliberately
 * does not keep one: a device that never traces should not carry the storage.
 *
 * `name` groups the operation in Sentry and must outlive the transaction — a string
 * literal, not a stack buffer. `op` is a coarse category (`"device.operation"`) and may be
 * NULL. Joins the trace already adopted from the caller, or starts one.
 *
 * Returns false when the SDK is disabled, or when the active trace is explicitly *not*
 * sampled — in which case nothing is built rather than built and thrown away.
 */
bool sentry_transaction_start(sentry_transaction_t *txn, const char *name, const char *op);

/**
 * Open a child span. Returns NULL when the transaction is full or was never started.
 *
 * NULL is safe to pass to `sentry_span_finish()` and `sentry_span_set_attribute()`, so a
 * caller never has to check. Dropped spans are counted and tagged on the transaction —
 * a trace quietly missing spans reads as a complete picture of a simpler operation.
 */
sentry_span_t *sentry_transaction_start_child(
    sentry_transaction_t *txn, const char *op, const char *description);

/** Close a span. NULL is a no-op. Closing twice keeps the first end time. */
void sentry_span_finish(sentry_span_t *span);

/**
 * Attach a number to a span — free heap, RSSI, frame time in microseconds, millivolts.
 *
 * Enriches this trace with numbers. Note what it is *not*: Sentry's Application Metrics
 * (`count` / `gauge` / `distribution`) are a separate product on a separate envelope item,
 * independent of tracing, and this SDK does not implement them. Sentry's own guidance is
 * that span attributes are for enriching existing traces and are subject to trace
 * sampling, while metrics that must not be sampled away belong in Application Metrics.
 *
 * That distinction matters more here than on a server: the numbers a device most wants to
 * report — heap over time, RSSI, frame time — belong to no operation, and a span needs one
 * to hang off.
 *
 * Integer because everything worth measuring on a device is one, and because printf's
 * float support is an opt-in linker flag here.
 */
void sentry_span_set_attribute(sentry_span_t *span, const char *key, int64_t value);

/**
 * Finish the transaction and send it.
 *
 * Returns `SENTRY_SEND_UNAVAILABLE` with nothing sent when the device has never been told
 * the date. A duration has no server-side substitute — the server observes one moment and
 * a duration needs two — so a transaction without one would place real work at an
 * arbitrary time. Errors are unaffected: they remain reportable with no clock at all.
 *
 * Uses `SENTRY_MICRO_ENVELOPE_BUFFER_BYTES` of the caller's stack on top of `txn` itself.
 */
sentry_response_t sentry_transaction_finish(sentry_transaction_t *txn);

/**
 * Add to a counter — brownouts, BLE disconnects, OTA failures.
 *
 *     sentry_metric_count("ble.disconnect", 1, NULL);
 *
 * **This does not send.** It adds to a fixed table and returns, and the table rides the next
 * `sentry_flush()`. That is the difference that matters on this hardware: a transaction
 * posts inline and blocks the loop task, so a path running several times a second cannot be
 * traced at any sampling rate — but it can be counted.
 *
 * `name` and `unit` are not copied; pass literals. Values are integers, because printf's
 * float support is an opt-in linker flag here and everything worth counting is whole.
 *
 * A clock is required, though: every metric carries a timestamp. Until the device has been
 * told the date the table keeps accumulating and nothing is sent — a counter covering a
 * longer interval is still true, which is why these are held rather than dropped the way a
 * transaction's stale duration would be.
 */
void sentry_metric_count(const char *name, int64_t delta, const char *unit);

/**
 * Record the latest reading of something continuous — free heap, RSSI, frame time.
 *
 *     sentry_metric_gauge("device.free_heap", ESP.getFreeHeap(), "byte");
 *
 * Same table and same no-send guarantee as `sentry_metric_count()`. The newest value wins;
 * a gauge is a sample rather than a history.
 */
void sentry_metric_gauge(const char *name, int64_t value, const char *unit);

/**
 * Distinct metric names dropped because the table was full, since `sentry_init()`.
 *
 * The table bounds *names*, not calls, so this only moves when firmware uses more than
 * `SENTRY_MICRO_MAX_METRICS` of them. Non-zero means a number you asked for is missing
 * entirely rather than merely coarse.
 */
uint32_t sentry_metrics_dropped_count(void);

/**
 * Report a message — the ordinary, non-crash way to tell Sentry something happened.
 *
 *     sentry_capture_message(SENTRY_LEVEL_WARNING, "OTA aborted: bad signature");
 *
 * Builds the event, writes the envelope, and sends it, buffering for a later retry if
 * there is no route right now — the same path everything else takes. `message` is used
 * during the call and never retained, so a stack buffer is fine.
 *
 * Uses `SENTRY_MICRO_ENVELOPE_BUFFER_BYTES` of the calling stack, like `sentry_flush()`.
 * Sends inline, so on a slow link this blocks for as long as the transport takes: fine at
 * boot, worth thinking about from a render loop (see README).
 *
 * Subject to the throttle configured by `max_messages_per_minute` and
 * `message_repeat_window_ms`, which is why this returns `SENTRY_SEND_RATE_LIMITED`
 * without contacting anyone more often than you might expect. `sentry_suppressed_count()`
 * says how often.
 */
sentry_response_t sentry_capture_message(sentry_level_t level, const char *message);

/**
 * Captured messages dropped by the local throttle since `sentry_init()`.
 *
 * Distinct from `sentry_dropped_count()`, which counts envelopes evicted from a full
 * buffer. These never became envelopes at all.
 */
uint32_t sentry_suppressed_count(void);

/**
 * Deliver a fully-built envelope through the registered transport.
 *
 * Fills in the ingest URL and the auth header from SDK state, so callers never assemble
 * either by hand — the transport is handed exactly what it needs and nothing it could get
 * wrong. Returns `SENTRY_SEND_UNAVAILABLE` when the SDK is disabled or no transport is
 * attached, which is the same "buffer it and retry" signal a downed radio produces.
 */
sentry_response_t sentry_send_envelope(const uint8_t *envelope, size_t len);

/**
 * Attach the stored core dump to `event`, if there is one.
 *
 * `storage` is filled in and pointed at by the event, so it must outlive the event — pass
 * a local in the same scope. Nothing is allocated.
 *
 * Also raises the event to `fatal` and sets a message describing the crash, since an event
 * carrying a backtrace is by definition not routine. Returns true when a crash was attached.
 *
 * Do **not** call `sentry_coredump_erase()` until the resulting envelope has been delivered
 * or buffered: erase early and a failed send loses the crash; never erase and every boot
 * re-reports it.
 */
bool sentry_event_attach_coredump(sentry_event_t *event, sentry_coredump_t *storage);

/**
 * Turn on offline buffering, backed by `storage`.
 *
 * Once enabled, an envelope that cannot be delivered is persisted instead of dropped, and
 * `sentry_flush()` retries it later. This is what lets the most valuable event the SDK
 * produces — the report of the crash that just happened, built at boot before the radio is
 * up — actually survive to be sent.
 *
 * `storage` is stored by pointer and must outlive the SDK. Returns false if the storage is
 * unusable, in which case the SDK carries on unbuffered rather than refusing to run.
 */
bool sentry_enable_buffering(const sentry_storage_t *storage);

/** Envelopes waiting to be delivered. 0 when buffering is off. */
uint32_t sentry_buffered_count(void);

/** Envelopes evicted because the buffer filled up, since the counter was last reported. */
uint32_t sentry_dropped_count(void);

/**
 * Try to deliver up to `max_events` buffered envelopes, oldest first.
 *
 * Stops at the first one that does not go through, so a downed network costs a single
 * failed attempt rather than one per queued event. Respects any backoff the server asked
 * for. Safe and cheap to call from `loop()`; it returns immediately when there is nothing
 * to do or the backoff has not expired.
 *
 * Uses `SENTRY_MICRO_ENVELOPE_BUFFER_BYTES` of the calling stack. Returns how many were
 * delivered.
 */
uint32_t sentry_flush(uint32_t max_events);

#ifdef __cplusplus
} /* extern "C" */

#    include "sentry_micro.hpp"
#endif

#endif /* SENTRY_MICRO_H_INCLUDED */
