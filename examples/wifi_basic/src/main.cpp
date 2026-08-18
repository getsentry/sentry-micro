/*
 * sentry-micro — wifi_basic
 *
 * The smallest useful end-to-end check: join a WiFi network, initialise sentry-micro from
 * a DSN, and print what the SDK worked out about this boot — the ingest endpoint it will
 * POST to, the auth header it will send, and why the chip came up (poweron? panic?
 * brownout? watchdog?).
 *
 * It then builds an event describing this boot, prints the envelope verbatim, and POSTs it
 * to Sentry over WiFi. Printing as well as sending is deliberate: ingest answers `200`
 * without telling the device what it accepted, so having the exact bytes on the console is
 * what makes a rejected or silently-dropped event diagnosable.
 *
 * Setup:
 *   cp src/secrets.example.h src/secrets.h   # then edit it
 *   pio run -e esp32dev -t upload -t monitor
 */

#include <Arduino.h>
#include <WiFi.h>

#include <device/sentry_storage_nvs.h>
#include <sentry_micro.h>
#include <transport/sentry_transport_auto.hpp>
#include <transport/sentry_transport_serial.hpp>
#include <transport/sentry_transport_wifi.hpp>

#include "certs.h"

/* Local, gitignored credentials. Absent in CI, where the values come from -D flags. */
#if __has_include("secrets.h")
#    include "secrets.h"
#endif

#ifndef WIFI_SSID
#    define WIFI_SSID "your-wifi-ssid"
#endif
#ifndef WIFI_PASSWORD
#    define WIFI_PASSWORD "your-wifi-password"
#endif
#ifndef SENTRY_DSN
#    define SENTRY_DSN "https://examplePublicKey@o0.ingest.sentry.io/0"
#endif
#ifndef SENTRY_MICRO_DEBUG
#    define SENTRY_MICRO_DEBUG 0
#endif

/* Build with -D SENTRY_DEMO_CRASH=1 to deliberately crash once per power-on, for testing
 * the whole crash -> reboot -> report -> symbolicate path. Off by default, obviously. */
#ifndef SENTRY_DEMO_CRASH
#    define SENTRY_DEMO_CRASH 0
#endif

/* Build with -D SENTRY_DEMO_SCAN=1 to list visible networks on every boot, not just
 * after a failed connect. */
#ifndef SENTRY_DEMO_SCAN
#    define SENTRY_DEMO_SCAN 0
#endif

/* Identifies this build in Sentry. Set by scripts/release.sh so the release the firmware
 * reports is the same one the debug files were uploaded under; the fallback only applies to
 * a plain `pio run`. */
#ifndef SENTRY_RELEASE
#    define SENTRY_RELEASE "sentry-micro-example@" SENTRY_MICRO_VERSION
#endif
#define FIRMWARE_RELEASE SENTRY_RELEASE

/* Attached as a tag, so a fleet of mixed hardware stays separable in the issue stream.
 * PlatformIO defines BOARD for every env, which is exactly the identifier we want. */
#ifndef BOARD_NAME
#    ifdef ARDUINO_BOARD
#        define BOARD_NAME ARDUINO_BOARD
#    else
#        define BOARD_NAME "unknown"
#    endif
#endif

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

/* File scope, not locals: the SDK stores a pointer, so these have to outlive setup(). */
static sentry::WiFiTransport wifi_transport;
static sentry::SerialTransport serial_transport;
/* WiFi first: it can actually tell whether it is available. Serial always claims to be
 * (there is no way to detect a listener on a bare UART), so it has to go last — anything
 * placed after it would never be reached. */
static sentry::AutoTransport transport({ &wifi_transport, &serial_transport });

/**
 * List what the radio can actually see.
 *
 * Printed only when a connect fails, because the failure statuses are nearly useless on
 * their own: `WL_NO_SSID_AVAIL` covers "typo in the SSID", "out of range", and — the one
 * that catches people out — "the network is 5 GHz and this chip is 2.4 GHz only". A scan
 * distinguishes them in one line.
 */
static void report_visible_networks()
{
    /* A connect attempt left running holds the radio and makes the scan fail rather than
     * return an empty list. Drop it first, but keep the credentials for the next attempt. */
    WiFi.disconnect(false, false);
    delay(100);

    Serial.println("[wifi] scanning for 2.4GHz networks in range ...");
    /*
     * A longer dwell than the 300ms default, but not as long as you might want.
     *
     * A beacon interval is ~100ms, so a short dwell on a busy channel can miss an AP
     * entirely — which reads as "the network isn't there" when it is just quiet. The
     * ceiling is not our patience though: Arduino's scanNetworks() waits a hard-coded
     * 10000ms for the scan to finish and returns WIFI_SCAN_FAILED (-2) if it has not, no
     * matter what dwell it was asked for. An active scan visits ~13 channels, so anything
     * above ~750ms per channel exceeds that and *always* fails.
     *
     * This was not theoretical: 1000ms/chan meant ~13s of scanning against a 10s ceiling,
     * so the diagnostic failed 100% of the time — and because the failed call leaves
     * WIFI_SCANNING_BIT set, every retry afterwards returned -1 ("still running") and no
     * amount of retrying or resetting the radio could recover it. 500ms keeps the whole
     * scan near 6.5s, comfortably inside the ceiling, and still well above the default.
     */
    int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true, /*passive=*/false,
        /*max_ms_per_chan=*/500);

    /* A synchronous scan should not come back "running", but it does if the driver started
     * one behind our back. Waiting for it beats reporting a failure we can still recover
     * from — the results are equally good whoever started the scan. */
    if (found == WIFI_SCAN_RUNNING) {
        uint32_t waited = millis();
        while (millis() - waited < 15000) {
            delay(250);
            int complete = WiFi.scanComplete();
            if (complete >= 0) {
                found = complete;
                break;
            }
        }
    }

    if (found < 0) {
        /* Distinguished from "found nothing" on purpose: a failed scan says nothing about
         * what is on the air, and reporting it as "no networks" sends you hunting for an
         * antenna fault that isn't there. */
        Serial.printf("[wifi]   scan failed (%d: %s)\n", found,
            found == WIFI_SCAN_RUNNING ? "still running" : "scan error");
        return;
    }
    if (found == 0) {
        Serial.println("[wifi]   none found — out of range, or the antenna is unplugged.");
        Serial.println("[wifi]   note: ESP32 is 2.4GHz-only and cannot see a 5GHz network.");
        return;
    }
    bool target_seen = false;
    for (int i = 0; i < found; i++) {
        Serial.printf("[wifi]   %-32s ch%-3d %4ddBm %s\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
            (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
        if (WiFi.SSID(i) == WIFI_SSID) {
            target_seen = true;
        }
    }
    WiFi.scanDelete();

    /* Comparing against the scan we already have, rather than a second filtered scan —
     * the ssid-filter argument to scanNetworks() only exists on Arduino core 3.x. */
    Serial.printf(
        "[wifi]   \"%s\" was %sfound in that list.\n", WIFI_SSID, target_seen ? "" : "NOT ");
}

static bool connect_wifi()
{
    Serial.printf("[wifi] connecting to \"%s\" ...\n", WIFI_SSID);

    /* Station mode only. The default in Arduino is STA+AP on some cores, and an idle
     * SoftAP burns power and RAM for nothing here. */
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[wifi] failed after %lums (status %d)\n",
            (unsigned long)(millis() - started), (int)WiFi.status());
        report_visible_networks();
        return false;
    }

    Serial.printf("[wifi] connected: ip=%s rssi=%ddBm in %lums\n",
        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned long)(millis() - started));
    return true;
}

static void print_sentry_state()
{
    const sentry_device_info_t &dev = sentry::device_info();
    const sentry_dsn_t &dsn = sentry::dsn();

    Serial.println();
    Serial.println("── sentry-micro ──────────────────────────────");
    Serial.printf("sdk           : %s %s\n", SENTRY_MICRO_SDK_NAME, sentry::sdk_version());
    Serial.printf("enabled       : %s\n", sentry::is_enabled() ? "yes" : "no");
    Serial.printf("release       : %s\n", FIRMWARE_RELEASE);
    Serial.printf("ingest host   : %s (tls: %s)\n", dsn.host, dsn.is_secure ? "yes" : "no");
    Serial.printf("project       : %s\n", dsn.project_id);
    /* Empty on self-hosted or a custom ingest domain — set Options::org_id there. */
    Serial.printf("org           : %s\n", sentry::org_id()[0] ? sentry::org_id() : "(unknown)");
    Serial.printf("envelope url  : %s\n", sentry::envelope_url());
    Serial.printf("auth header   : %s\n", sentry::auth_header());
    Serial.println("── device ────────────────────────────────────");
    Serial.printf("chip          : %s rev %u, %u core(s)\n", dev.chip_model,
        (unsigned)dev.chip_revision, (unsigned)dev.cpu_cores);
    Serial.printf("device id     : %s\n", dev.device_id);
    Serial.printf("idf           : %s\n", dev.sdk_version);
    Serial.printf("flash         : %u KB\n", (unsigned)(dev.flash_size_bytes / 1024));
    Serial.printf("heap          : %u / %u KB free (min %u KB)\n",
        (unsigned)(sentry_device_free_heap() / 1024), (unsigned)(dev.total_heap_bytes / 1024),
        (unsigned)(sentry_device_min_free_heap() / 1024));
    Serial.printf("reset reason  : %s%s\n", sentry_reset_reason_name(dev.reset_reason),
        sentry_reset_reason_is_crash(dev.reset_reason) ? "  <- previous boot crashed" : "");
    Serial.println("──────────────────────────────────────────────");
    Serial.println();
}

/** Human-readable name for a delivery result, for the serial log. */
static const char *send_result_name(sentry_send_result_t result)
{
    switch (result) {
    case SENTRY_SEND_OK:
        return "delivered";
    case SENTRY_SEND_UNAVAILABLE:
        return "no route (buffer and retry)";
    case SENTRY_SEND_REJECTED:
        return "rejected (do not retry)";
    case SENTRY_SEND_RATE_LIMITED:
        return "rate limited";
    case SENTRY_SEND_ERROR:
    default:
        return "error";
    }
}

/**
 * Build the envelope describing this boot, print it, and send it.
 *
 * Printing the bytes as well as sending them is deliberate: when ingest answers 200 the
 * device learns nothing about *what* it sent, so having the exact payload on the console
 * is what makes a rejected or silently-dropped event diagnosable.
 */
#if SENTRY_DEMO_CRASH
/*
 * A deliberate null-pointer store, three calls deep.
 *
 * `noinline` on each level so the optimiser cannot collapse them into one frame — the point
 * is to produce a backtrace with several entries to symbolicate. `volatile` stops the store
 * being discarded as dead code, which at -Os it otherwise would be.
 */
static void __attribute__((noinline)) demo_crash_innermost()
{
    volatile uint32_t *nowhere = (volatile uint32_t *)0;
    *nowhere = 0xdeadbeef;
}

static void __attribute__((noinline)) demo_crash_middle() { demo_crash_innermost(); }

static void __attribute__((noinline)) demo_crash_outer() { demo_crash_middle(); }
#endif

/**
 * Report a crash recovered from the core dump partition, if there is one.
 *
 * Returns true if a crash was reported, so the caller knows not to immediately cause
 * another one.
 */
static bool report_crash()
{
    char event_id[SENTRY_MICRO_EVENT_ID_LEN];
    sentry_event_t event;
    sentry_coredump_t crash;

    if (!sentry_event_prepare(&event, event_id)) {
        return false;
    }
    /* `crash` has to outlive the event, which points into it — hence both being locals of
     * this function rather than the event owning a copy. */
    if (!sentry_event_attach_coredump(&event, &crash)) {
        return false;
    }

    char message[128];
    snprintf(message, sizeof(message), "%s in task %s",
        crash.exception_type[0] ? crash.exception_type : "Panic", crash.task_name);
    event.message = message;

    Serial.println();
    Serial.println("── crash recovered from the previous boot ────");
    Serial.printf("exception   : %s\n", crash.exception_type);
    Serial.printf("task        : %s\n", crash.task_name);
    Serial.printf("pc          : 0x%08x\n", (unsigned)crash.exception_pc);
    if (crash.exception_addr_valid) {
        Serial.printf("accessing   : 0x%08x\n", (unsigned)crash.exception_addr);
    }
    Serial.printf("backtrace   : %u frames%s\n", (unsigned)crash.frame_count,
        crash.truncated ? " (truncated)" : "");
    for (uint32_t i = 0; i < crash.frame_count; i++) {
        Serial.printf("  #%u 0x%08x\n", (unsigned)i, (unsigned)crash.frames[i]);
    }
    Serial.println("──────────────────────────────────────────────");

    char envelope[2048];
    size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
    if (needed == 0 || needed >= sizeof(envelope)) {
        Serial.printf("[sentry] crash envelope needs %u bytes\n", (unsigned)needed);
        return true;
    }

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
    Serial.printf("[sentry] crash report: %s (http %u)\n", send_result_name(response.result),
        (unsigned)response.http_status);

    /* Erase only once the envelope is somewhere durable — delivered, or in the buffer.
     * Erasing on a plain failure would throw away the crash we just recovered. */
    if (response.result == SENTRY_SEND_OK || sentry_buffered_count() > 0) {
        sentry_coredump_erase();
        Serial.println("[sentry] core dump erased");
    } else {
        Serial.println("[sentry] core dump kept for the next boot");
    }
    Serial.printf("[sentry] crash event %s\n", event_id);
    return true;
}

static void report_boot()
{
    char event_id[SENTRY_MICRO_EVENT_ID_LEN];
    sentry_event_t event;
    if (!sentry_event_prepare(&event, event_id)) {
        Serial.println("[sentry] could not prepare an event (SDK disabled?)");
        return;
    }

    const sentry_device_info_t &dev = sentry::device_info();
    bool crashed = sentry_reset_reason_is_crash(dev.reset_reason);

    char message[96];
    snprintf(
        message, sizeof(message), "Device booted: %s", sentry_reset_reason_name(dev.reset_reason));
    event.message = message;
    /* A crash is fatal; an ordinary power-on is just news. Getting this wrong would either
     * page someone for a normal reboot or bury a real panic. */
    event.level = crashed ? SENTRY_LEVEL_FATAL : SENTRY_LEVEL_INFO;

    /* Stack-allocated: the no-allocation rule applies to the reporting path, and 2 KB of
     * an 8 KB loop-task stack is affordable where a fragmented heap may not be. */
    char envelope[2048];
    size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
    if (needed == 0 || needed >= sizeof(envelope)) {
        Serial.printf("[sentry] envelope needs %u bytes, buffer is %u\n", (unsigned)needed,
            (unsigned)sizeof(envelope));
        return;
    }

    Serial.printf("── envelope (%u bytes) ───────────────────────\n", (unsigned)needed);
    Serial.print(envelope);
    Serial.println("──────────────────────────────────────────────");

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
    Serial.printf(
        "[sentry] %s (http %u", send_result_name(response.result), (unsigned)response.http_status);
    if (response.retry_after_ms > 0) {
        Serial.printf(", retry after %ums", (unsigned)response.retry_after_ms);
    }
    Serial.println(")");

    if (response.result == SENTRY_SEND_OK) {
        Serial.printf("[sentry] event %s is in Sentry\n", event_id);
    }
}

void setup()
{
    Serial.begin(115200);
    /* Native-USB parts (S2/S3/C3/C6) enumerate their CDC port well after boot; without
     * this the first few hundred milliseconds of output are lost to the host. */
    uint32_t serial_wait = millis();
    while (!Serial && millis() - serial_wait < 3000) {
        delay(10);
    }
    Serial.println("\n\nsentry-micro wifi_basic example");

    /*
     * Initialise before anything else that might crash. The reset reason read here belongs
     * to the *previous* boot, so any panic that happens before init() is invisible — the
     * earlier this runs, the more of the firmware it covers.
     */
    sentry::Options options;
    options.dsn = SENTRY_DSN;
    options.release = FIRMWARE_RELEASE;
    options.environment = "development";
    options.board = BOARD_NAME;
    options.debug = SENTRY_MICRO_DEBUG;

    if (!sentry::init(options)) {
        /* A bad DSN disables the SDK rather than half-configuring it; the firmware is
         * expected to carry on regardless. Crash reporting must never be load-bearing. */
        Serial.println("[sentry] init failed — check SENTRY_DSN in src/secrets.h");
    }

    /*
     * Buffering, before anything is sent. Eight slots of NVS is roughly 6 KB of the stock
     * 20 KB partition. Anything that cannot be delivered now is persisted and retried by
     * sentry_flush() below, which is what makes a boot-time crash report survive having no
     * network at the moment it is created.
     */
    if (!sentry_enable_buffering(sentry_storage_nvs(8))) {
        Serial.println("[sentry] NVS unavailable — running without an offline buffer");
    }
    Serial.printf("[sentry] %u envelope(s) buffered from a previous run, %u dropped\n",
        (unsigned)sentry_buffered_count(), (unsigned)sentry_dropped_count());

    print_sentry_state();

#if SENTRY_DEMO_SCAN
    /* Build with -D SENTRY_DEMO_SCAN=1 to list what the radio can see on every boot,
     * whether or not the connect succeeds. Normally this only runs after a failure, which
     * makes the scan itself awkward to test — and a diagnostic nobody can exercise is how
     * it came to be broken (see report_visible_networks). */
    report_visible_networks();
#endif

    /*
     * Turn on TLS verification before any send can happen. Without set_ca_cert() the
     * transport sends encrypted but *unauthenticated* — a device will talk to any server
     * that answers its DSN host. The root in certs.h verifies the public Sentry cloud out
     * of the box; swap in your ingest host's root instead if you self-host (see certs.h).
     *
     * Unconditional, not inside a "did WiFi come up?" branch: `transport` re-picks a route
     * on every delivery attempt, so WiFi may be chosen long after setup() has returned.
     */
    wifi_transport.set_ca_cert(SENTRY_INGEST_CA_CERT);

    /*
     * Join WiFi if possible — connect_wifi() prints the outcome either way, and runs a
     * scan diagnostic on failure. Its return value is not used to pick a transport, unlike
     * before: `transport` (file scope, declared above) already tries WiFi first and falls
     * back to the serial relay on every delivery attempt, re-evaluated fresh each time —
     * so a WiFi connection that comes up *after* this point still gets used, with no
     * reboot and no code here watching for the transition.
     */
    connect_wifi();
    sentry::set_transport(transport);
    Serial.println("[sentry] transport: auto (wifi with tls verification, falling back to "
                   "the serial relay — run scripts/serial_relay.py if wifi is unreachable)");

    /* A recovered crash is the more interesting event, and reporting both on the same boot
     * would double up. */
    if (!report_crash()) {
        report_boot();
    }

#if SENTRY_DEMO_CRASH
    /* Crash only when this boot did *not* follow a crash, so one power-on produces exactly
     * one crash-and-report cycle instead of an endless loop. */
    if (!sentry_reset_reason_is_crash(sentry::device_info().reset_reason)) {
        Serial.println("\n[demo] crashing deliberately in 3s ...");
        delay(3000);
        demo_crash_outer();
    }
#endif
}

void loop()
{
    /* Once a minute, show that the device is alive and what its resources look like —
     * the same numbers that will ride along on every event as device context. */
    /* Retry anything the buffer is holding — this is how an event created before the radio
     * came up eventually gets out.
     *
     * On an interval, not every iteration: a transport can block for seconds when there is
     * no route (the serial relay waits for a host that may not be listening), so flushing
     * every pass would turn `loop()` into a chain of timeouts. */
    static uint32_t last_flush = 0;
    if (sentry_buffered_count() > 0 && millis() - last_flush >= 30000) {
        last_flush = millis();
        sentry_flush(2);
    }

    static uint32_t last_report = 0;
    if (millis() - last_report >= 60000) {
        last_report = millis();
        Serial.printf("[heartbeat] up %llus, heap %uKB free (min %uKB), wifi %s rssi %ddBm\n",
            (unsigned long long)(sentry_device_uptime_ms() / 1000),
            (unsigned)(sentry_device_free_heap() / 1024),
            (unsigned)(sentry_device_min_free_heap() / 1024),
            WiFi.status() == WL_CONNECTED ? "up" : "down", (int)WiFi.RSSI());
    }
    delay(50);
}
