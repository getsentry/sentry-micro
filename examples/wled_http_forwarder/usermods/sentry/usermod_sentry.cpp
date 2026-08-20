/*
 * sentry usermod — WLED + sentry-micro, HTTP-forwarder push (PoC)
 *
 * The crash-reporting path this exercises: ESP-IDF's panic handler writes a coredump to
 * flash and reboots; on the next boot this usermod decodes it, builds an envelope, and
 * hands it to sentry-micro's WiFiTransport pointed at a plain-HTTP forwarder — no TLS on
 * the device at all. If delivery fails, the envelope stays in the NVS-backed buffer and
 * is retried on the next `sentry_flush()` tick, same as it would for a real ingest URL.
 *
 * The forwarder itself is `http_forwarder.py`, alongside this usermod in this example —
 * a stateless plain HTTP -> HTTPS relay with no Sentry-specific knowledge. See that
 * script and this example's README for how the two are supposed to fit together, in
 * particular how the on-device "DSN" is deliberately not a real one.
 *
 * This file is the whole usermod: WLED's v2 usermods self-register via REGISTER_USERMOD
 * (a linker-section trick, see wled00/fcn_declare.h) and don't require touching any WLED
 * source file, including usermods_list.cpp from older WLED versions, which doesn't exist
 * in this codebase anymore.
 *
 * On-demand crash trigger: GET /sentry/crash. Deliberately not "crash N seconds after
 * boot" (wifi_basic's SENTRY_DEMO_CRASH pattern) — that would fight normal use of an
 * actual lighting controller. The handler responds 200 immediately and defers the crash
 * to the next loop() tick, so the HTTP response has a chance to flush before the reboot.
 *
 * Coredump support depends entirely on which Arduino-ESP32 core build this links
 * against — see platformio_override.ini's `platform =` line and its comment before
 * assuming a real backtrace will show up. WLED's own default platform pin (a Tasmota
 * fork of platform-espressif32) ships CONFIG_ESP_COREDUMP_ENABLE_TO_NONE across its
 * entire board matrix, baked into a precompiled Arduino core — no build flag fixes that.
 * The `[sentry] coredump: supported=.. available=..` line this usermod prints at boot is
 * the fastest way to tell which state a given build is in.
 */

#include "wled.h"

#include <atomic>
#include <cstdlib>

#include <device/sentry_coredump_device.h>
#include <device/sentry_storage_nvs.h>
#include <sentry_micro.h>
#include <transport/sentry_transport_wifi.hpp>

/*
 * The DSN here is not a real Sentry DSN — see sentry_dsn.h, which parses
 * `<scheme>://<public_key>@<host>[:<port>]/<project_id>` generically, with no requirement
 * that `host` resolve to Sentry at all. Run http_forwarder.py with the test project's
 * real DSN and it prints the value to use here: same public key and project id, host
 * replaced with wherever the forwarder is listening. The device then POSTs plain HTTP to
 * the forwarder, which relays to the real ingest host over HTTPS — see that script's own
 * header comment for why it does not need to know the public key or project id at all.
 */
#ifndef SENTRY_DSN
#    define SENTRY_DSN "http://examplePublicKey@192.0.2.1:8080/0"
#endif

#ifndef SENTRY_MICRO_DEBUG
#    define SENTRY_MICRO_DEBUG 1
#endif

#define SENTRY_UM_STRINGIFY_(x) #x
#define SENTRY_UM_STRINGIFY(x) SENTRY_UM_STRINGIFY_(x)

/*
 * scripts/release.sh sets SENTRY_MICRO_RELEASE (-> this macro, via env_secrets.py) to
 * whatever it registers with Sentry and stamped the matching debug file under — using
 * anything else here would report events under a release Sentry never heard of. Falls
 * back to WLED's own version for a plain `pio run` with no release.sh involved, same as
 * before; that build still works, it just has no debug file to symbolicate against.
 */
#ifndef SENTRY_RELEASE
#    define SENTRY_RELEASE "wled@" SENTRY_UM_STRINGIFY(VERSION)
#endif

namespace {

/*
 * A deliberate abort() rather than a null-pointer write: the latter (same shape as
 * examples/wifi_basic's demo_crash_*) does not reliably fault on every toolchain this
 * project might build against, since whether it faults depends on region-protection
 * configuration that varies by platform pin. abort() triggers ESP-IDF's panic handler
 * unconditionally, independent of that configuration, and still produces a coredump the
 * same way any other fatal panic does.
 *
 * Still three calls deep and noinline, so the backtrace has multiple frames to
 * symbolicate.
 */
void __attribute__((noinline)) sentryDemoCrashInnermost() { abort(); }
void __attribute__((noinline)) sentryDemoCrashMiddle() { sentryDemoCrashInnermost(); }
void __attribute__((noinline)) sentryDemoCrashOuter() { sentryDemoCrashMiddle(); }

} // namespace

class UsermodSentry : public Usermod {
public:
    void setup() override
    {
        sentry::Options options;
        options.dsn = SENTRY_DSN;
        options.release = SENTRY_RELEASE;
        options.environment = "development";
        options.board = ARDUINO_BOARD;
        options.debug = SENTRY_MICRO_DEBUG;

        if (!sentry::init(options)) {
            /* A bad DSN disables the SDK, not the firmware — crash reporting must never
             * be load-bearing for a lighting controller. */
            DEBUG_PRINTLN(F("[sentry] init failed — check SENTRY_DSN"));
        }

        /* Sixteen slots of NVS, same as wifi_basic. Anything the forwarder can't take right
         * now survives here and is retried by loop()'s sentry_flush() below — including
         * across a reboot, which is the case this whole design exists for. */
        if (!sentry_enable_buffering(sentry_storage_nvs(16))) {
            DEBUG_PRINTLN(F("[sentry] NVS unavailable — running without an offline buffer"));
        }

        /* No TLS anywhere in this build (SENTRY_MICRO_WIFI_TLS=0, see
         * platformio_override.ini) — set_ca_cert() would not compile in that
         * configuration, and there is no HTTPS branch to configure. */
        sentry::set_transport(wifiTransport_);

        /* Registered here rather than gated behind WLED_CONNECTED: the handler itself
         * only sets a flag, so it is harmless to register before WiFi is up, and doing it
         * unconditionally in setup() means it does not need re-registering in connected(). */
        server.on("/sentry/crash", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->send(200, "text/plain",
                "Crashing in 500ms — watch the serial console for the recovered "
                "backtrace on next boot.\n");
            /* Deadline set before the pending flag, not after: loop() runs on the other
             * core and only checks crashAtMs_ once it observes crashPending_ true, so this
             * order guarantees it never reads a stale (default-zero) deadline and fires
             * immediately instead of after the 500ms wait. */
            crashAtMs_ = millis() + 500;
            crashPending_ = true;
        });

        /* A recovered crash is the more interesting event; reporting both on the same
         * boot would double up. */
        DEBUG_PRINTF("[sentry] coredump: supported=%d available=%d\n",
            (int)sentry_coredump_is_supported(), (int)sentry_coredump_available());
        if (!reportCrash()) {
            reportBoot();
        }
    }

    void loop() override
    {
        if (crashPending_ && millis() >= crashAtMs_) {
            crashPending_ = false;
            sentryDemoCrashOuter();
            /* Should never print — abort() panics unconditionally. If it does, this
             * toolchain's abort()-triggers-panic assumption no longer holds and the
             * crash trigger needs re-verifying. */
            DEBUG_PRINTLN(F("[sentry] crash call RETURNED — no fault occurred"));
        }

        /* On an interval, not every pass: WiFiTransport::send() blocks for up to its
         * timeout when the forwarder is unreachable, and flushing every loop() call would
         * turn WLED's frame rate into a chain of those timeouts. */
        static uint32_t lastFlushMs = 0;
        if (sentry_buffered_count() > 0 && millis() - lastFlushMs >= 30000) {
            lastFlushMs = millis();
            sentry_flush(2);
        }
    }

private:
    /**
     * Report a crash recovered from the coredump partition, if there is one.
     * Mirrors examples/wifi_basic's report_crash() exactly — see that file for the
     * envelope-building rationale.
     */
    bool reportCrash()
    {
        char eventId[SENTRY_MICRO_EVENT_ID_LEN];
        sentry_event_t event;
        sentry_coredump_t crash;

        if (!sentry_event_prepare(&event, eventId)) {
            return false;
        }
        if (!sentry_event_attach_coredump(&event, &crash)) {
            return false;
        }

        char message[128];
        snprintf(message, sizeof(message), "%s in task %s",
            crash.exception_type[0] ? crash.exception_type : "Panic", crash.task_name);
        event.message = message;

        DEBUG_PRINTLN(F("[sentry] crash recovered from the previous boot"));
        DEBUG_PRINTF("[sentry] exception %s at pc 0x%08x, %u frame(s)%s\n", crash.exception_type,
            (unsigned)crash.exception_pc, (unsigned)crash.frame_count,
            crash.truncated ? " (truncated)" : "");

        char envelope[2048];
        size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
        if (needed == 0 || needed >= sizeof(envelope)) {
            DEBUG_PRINTF("[sentry] crash envelope needs %u bytes\n", (unsigned)needed);
            return true;
        }

        uint32_t bufferedBefore = sentry_buffered_count();
        sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
        DEBUG_PRINTF("[sentry] crash report result %d (http %u)\n", (int)response.result,
            (unsigned)response.http_status);

        /* Erase only once *this* envelope is somewhere durable — delivered, or newly
         * buffered. sentry_buffered_count() > 0 alone isn't enough to tell that apart from
         * a buffer that already held older entries: if this envelope fails to send and
         * also fails to enqueue (buffer full, storage error), that check still passes on
         * the old entries alone, and erasing here would lose the crash just recovered
         * without it ever landing anywhere. Comparing against the count taken before the
         * send call is the only way to tell "queue has stuff in it" apart from "my
         * envelope is the reason it does" without sentry_send_envelope() itself
         * distinguishing the two outcomes. */
        bool durablyHeld
            = response.result == SENTRY_SEND_OK || sentry_buffered_count() > bufferedBefore;
        if (durablyHeld) {
            sentry_coredump_erase();
            DEBUG_PRINTLN(F("[sentry] coredump erased"));
        } else {
            DEBUG_PRINTLN(F("[sentry] coredump kept for the next boot"));
        }
        return true;
    }

    void reportBoot()
    {
        const sentry_device_info_t &dev = sentry::device_info();
        bool crashed = sentry_reset_reason_is_crash(dev.reset_reason);

        char message[96];
        snprintf(message, sizeof(message), "WLED booted: %s",
            sentry_reset_reason_name(dev.reset_reason));

        sentry_response_t response
            = sentry::capture_message(crashed ? SENTRY_LEVEL_FATAL : SENTRY_LEVEL_INFO, message);
        DEBUG_PRINTF("[sentry] boot report result %d (http %u)\n", (int)response.result,
            (unsigned)response.http_status);
    }

    /* File/instance scope, not a local: the SDK stores a pointer to this, so it has to
     * outlive the transport being registered — see sentry::set_transport()'s contract.
     * The usermod instance itself is a static global (below), so this is fine. */
    sentry::WiFiTransport wifiTransport_;

    /* atomic, not plain bool/uint32_t: written from AsyncTCP's own FreeRTOS task (which
     * services server.on() handlers) and read from the main loop() task — on a dual-core
     * ESP32 those can be different cores, and a plain write has no guaranteed visibility
     * or ordering across that boundary. Same reasoning as RelayTransport's
     * status_pending_/status_response_ pair (src/transport/sentry_transport_relay.hpp). */
    std::atomic<bool> crashPending_ { false };
    std::atomic<uint32_t> crashAtMs_ { 0 };
};

static UsermodSentry sentryUsermod;
REGISTER_USERMOD(sentryUsermod);
