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
#include <time.h>
#include <WiFi.h>

#include <device/sentry_storage_nvs.h>
#include <sentry_micro.h>
#include <transport/sentry_transport_auto.hpp>
#include <transport/sentry_transport_serial.hpp>
#include <transport/sentry_transport_wifi.hpp>

#if SENTRY_MICRO_WIFI_TLS
#    include "certs.h"
#endif

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

/* Build with -D SENTRY_DEMO_FLOOD=1 to watch the capture throttle work. */
#ifndef SENTRY_DEMO_FLOOD
#    define SENTRY_DEMO_FLOOD 0
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
static const uint32_t TIME_SYNC_TIMEOUT_MS = 20000;
/* Matches the guard in sentry_device_unix_time_us(): below this, the clock has not been
 * set and is not a real time, whatever it reads. */
static const time_t TIME_SYNC_PLAUSIBLE_EPOCH_S = 1600000000;

/* File scope, not locals: the SDK stores a pointer, so these have to outlive setup(). */
static sentry::WiFiTransport wifi_transport;
static sentry::SerialTransport serial_transport;
/* WiFi first: it can actually tell whether it is available. Serial always claims to be
 * (there is no way to detect a listener on a bare UART), so it has to go last — anything
 * placed after it would never be reached. */
static sentry::AutoTransport transport({ &wifi_transport, &serial_transport });

/* Set once sync_time() succeeds; read in loop() to decide whether to keep retrying. File
 * scope for the same reason as the transports above: setup() only gets one attempt at
 * this, but a WiFi connection that comes up after setup() has returned still needs it. */
static bool time_synced = false;

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
        /* Called from inside whatever trace the caller started (see setup()) — sentry_log()
         * reads that state itself, so this line needs no trace argument to end up attached
         * to it. */
        sentry::log(SENTRY_LEVEL_WARNING, "WiFi connect failed after %lums (status %d)",
            (unsigned long)(millis() - started), (int)WiFi.status());
        report_visible_networks();
        return false;
    }

    Serial.printf("[wifi] connected: ip=%s rssi=%ddBm in %lums\n",
        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned long)(millis() - started));
    sentry::log(SENTRY_LEVEL_INFO, "WiFi connected: ip=%s rssi=%ddBm in %lums",
        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned long)(millis() - started));
    return true;
}

/**
 * Sync the system clock via SNTP, blocking until it reaches a plausible time or times out.
 *
 * An ESP32 has no battery-backed clock: coming out of reset it believes it is 1970, which
 * is before every real certificate's validity window. TLS fails the moment it checks that
 * window, well before anything Sentry-specific runs — so this has to happen before the
 * first send attempt, not just before whatever in this SDK cares about a timestamp.
 *
 * Returns true once the clock is plausible, so a caller with no route yet (WiFi not up)
 * can retry later instead of leaving TLS broken for the rest of the boot — see the retry in
 * loop().
 */
static bool sync_time()
{
    Serial.print("[time] syncing via SNTP ...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    time_t now = 0;
    uint32_t started = millis();
    while (now < TIME_SYNC_PLAUSIBLE_EPOCH_S && millis() - started < TIME_SYNC_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
        time(&now);
    }
    Serial.println();

    if (now < TIME_SYNC_PLAUSIBLE_EPOCH_S) {
        Serial.println("[time] sync failed — TLS verification and any timestamped Sentry "
                       "item (transaction, metric) will not go through until it succeeds");
        return false;
    }
    Serial.printf("[time] synced: %s", asctime(gmtime(&now)));
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

    /* The only place left in this example that shows the raw ndjson, now that the boot
     * report goes through sentry::capture_message(). Worth seeing once: it is the whole
     * protocol, and any transport that can move these bytes is a complete implementation. */
    Serial.printf("── envelope (%u bytes) ───────────────────────\n", (unsigned)needed);
    Serial.print(envelope);
    Serial.println("──────────────────────────────────────────────");

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
    const sentry_device_info_t &dev = sentry::device_info();
    bool crashed = sentry_reset_reason_is_crash(dev.reset_reason);

    char message[96];
    snprintf(
        message, sizeof(message), "Device booted: %s", sentry_reset_reason_name(dev.reset_reason));

    /* The whole event, in one call. Everything report_crash() above does by hand — fresh
     * event id, release, device context, live heap and uptime, envelope framing, send,
     * buffer on failure — happens inside here. Build with `options.debug` to see the id.
     *
     * A crash is fatal; an ordinary power-on is just news. Getting this wrong would either
     * page someone for a normal reboot or bury a real panic. */
    sentry_response_t response
        = sentry::capture_message(crashed ? SENTRY_LEVEL_FATAL : SENTRY_LEVEL_INFO, message);

    Serial.printf(
        "[sentry] %s (http %u", send_result_name(response.result), (unsigned)response.http_status);
    if (response.retry_after_ms > 0) {
        Serial.printf(", retry after %ums", (unsigned)response.retry_after_ms);
    }
    Serial.println(")");
}

/**
 * Show what the throttle does, because a limiter you cannot see is one you cannot trust.
 *
 * Build with `-D SENTRY_DEMO_FLOOD=1`. It captures the same message twenty times in a
 * tight loop — a failing sensor read, more or less exactly — and then a different one, to
 * make the point that suppressing a repeat does not cost the budget some *other* message
 * needed. Expect one event from the first twenty, and one from the last call.
 */
#if SENTRY_DEMO_FLOOD
static void demo_flood()
{
    Serial.println();
    Serial.println("── throttle demo ─────────────────────────────");

    uint32_t captured = 0;
    for (int i = 0; i < 20; i++) {
        /* RATE_LIMITED is the throttle's answer. Any other result means the event was
         * built and handed to the transport, whether or not the radio was up — which is a
         * different question, and not the one this demo is about. */
        if (sentry::capture_message(SENTRY_LEVEL_WARNING, "Sensor read failed").result
            != SENTRY_SEND_RATE_LIMITED) {
            captured++;
        }
    }
    Serial.printf("20 identical captures -> %u became events, %u suppressed\n", (unsigned)captured,
        (unsigned)sentry::suppressed_count());

    sentry_response_t other = sentry::capture_message(SENTRY_LEVEL_WARNING, "Something else");
    Serial.printf("a different message   -> %s\n", send_result_name(other.result));
    Serial.println("──────────────────────────────────────────────");
}
#endif

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
     * Buffering, before anything is sent. Sixteen slots of NVS is roughly 12-16 KB of the
     * stock 20 KB partition (envelopes here run ~1 KB each) — this example's own choice for
     * its own partition, not a ceiling the SDK imposes; sentry_storage_nvs() accepts any
     * count up to SENTRY_NVS_MAX_SLOTS and a smaller firmware should pass a smaller one.
     * Anything that cannot be delivered now is persisted and retried by sentry_flush()
     * below, which is what makes a boot-time crash report survive having no network at the
     * moment it is created.
     *
     * One shared ring across every envelope type, oldest evicted first, with no priority
     * between them — a high-volume category left unattended for long enough can still
     * evict something that mattered more. The fix for that is a transport that keeps
     * delivering, not triage over whose envelope gets to keep its slot.
     */
    if (!sentry_enable_buffering(sentry_storage_nvs(16))) {
        Serial.println("[sentry] NVS unavailable — running without an offline buffer");
    }
    Serial.printf("[sentry] %u envelope(s) buffered from a previous run, %u dropped\n",
        (unsigned)sentry_buffered_count(), (unsigned)sentry_dropped_count());

    print_sentry_state();

    /* Recorded before any trace exists this boot — see sentry_log()'s doc for why an idle
     * line is still held and sent. Compare with the line inside connect_wifi() below,
     * recorded once the wifi-connect trace is active. */
    sentry::log(SENTRY_LEVEL_INFO, "%s starting on %s", FIRMWARE_RELEASE, BOARD_NAME);

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
#if SENTRY_MICRO_WIFI_TLS
    wifi_transport.set_ca_cert(SENTRY_INGEST_CA_CERT);
#else
    /* Built with -D SENTRY_MICRO_WIFI_TLS=0: there is no HTTPS branch to configure, and an
     * https DSN will be refused rather than sent in clear. Only useful against a
     * self-hosted endpoint on plain HTTP. */
    Serial.println("[sentry] built without TLS; the WiFi transport is plain HTTP only");
#endif

    /*
     * Registered before WiFi has necessarily even connected, not after: the wifi-connect
     * transaction below tries to send the moment it finishes, and `transport` is what
     * makes that possible rather than something merely buffered for later. AutoTransport
     * re-picks a route on every attempt regardless of when it was registered, so there is
     * no reason to wait for a connection first.
     */
    sentry::set_transport(transport);
    Serial.println("[sentry] transport: auto (wifi with tls verification, falling back to "
                   "the serial relay — run scripts/serial_relay.py if wifi is unreachable)");

    /*
     * Join WiFi if possible — connect_wifi() prints the outcome either way, and runs a
     * scan diagnostic on failure. Wrapped in its own transaction so the attempt is a traced
     * operation with a duration in Sentry, and so connect_wifi()'s own sentry_log() call
     * lands attached to it rather than to nothing — sentry_transaction_start() starts a
     * trace itself when none is active yet, which is what makes that attachment happen
     * with no trace object threaded through connect_wifi().
     *
     * The clock sync happens *before* the transaction is finished, not just before it
     * gates `time_synced` for loop()'s retry below: sentry_transaction_finish() needs a
     * synced clock to keep this transaction at all (see its doc), and the very first WiFi
     * connection of a boot is exactly the operation that makes a sync possible in the
     * first place. Finishing after it, rather than before, is what lets
     * this specific transaction get a real timestamp instead of being dropped every time.
     */
    sentry::Transaction wifi_txn;
    sentry::transaction_start(wifi_txn, "wifi-connect", "device.operation");
    bool wifi_connected = connect_wifi();
    if (wifi_connected) {
        time_synced = sync_time();
    }
    sentry::transaction_finish(wifi_txn);
    /* Released, not left active: sentry_transaction_finish() ends the transaction but not
     * the trace it rode, and anything reported past this point — the boot event below, a
     * later demo crash — has nothing to do with this WiFi connection. Leaving it active
     * would weld those unrelated events to it instead of leaving them untraced. */
    sentry::trace_release();

    /* A recovered crash is the more interesting event, and reporting both on the same boot
     * would double up. */
    if (!report_crash()) {
        report_boot();
    }

#if SENTRY_DEMO_FLOOD
    demo_flood();
#endif

#if SENTRY_DEMO_CRASH
    /* Crash only when this boot did *not* follow a crash, so one power-on produces exactly
     * one crash-and-report cycle instead of an endless loop. */
    if (!sentry_reset_reason_is_crash(sentry::device_info().reset_reason)) {
        Serial.println("\n[demo] crashing deliberately in 3s ...");
        /*
         * A trace, not a transaction that ever finishes: demo_crash_outer() ends the boot,
         * so there is no operation left to time or send. What matters is that a trace is
         * active when it dies — sentry_event_attach_coredump() (see report_crash(), next
         * boot) reads whatever trace was active at the moment of the crash and joins the
         * recovered event to it, the same way a request handler's trace would join a crash
         * that happened while it was running.
         */
        sentry::Transaction crash_txn;
        sentry::transaction_start(crash_txn, "demo-crash", "device.operation");
        sentry::log(SENTRY_LEVEL_FATAL, "Deliberately crashing in demo_crash_outer() in 3s");
        /* sentry_log() only writes into the ring — see its doc — and demo_crash_outer()
         * below ends the boot before loop() ever runs again to flush it on its own
         * interval. Flushed explicitly here so the line this demo exists to show actually
         * leaves the device instead of being zeroed with the rest of RAM on reset. */
        sentry_flush(4);
        delay(3000);
        demo_crash_outer();
    }
#endif
}

void loop()
{
    /* Retry the clock sync if the first attempt in setup() did not land — WiFi that
     * associates late (out of range for a few seconds, an AP mid-reboot) is exactly the
     * case `transport` above already handles by re-picking a route on every send, but a
     * synced clock is not re-derived the same way, so nothing else here revisits it. On an
     * interval and only while WiFi is actually up, for the same reason as the flush below:
     * sync_time() blocks for up to TIME_SYNC_TIMEOUT_MS when it fails, which is too long to
     * retry on every pass. */
    static uint32_t last_time_sync_attempt = 0;
    if (!time_synced && WiFi.status() == WL_CONNECTED
        && millis() - last_time_sync_attempt >= 30000) {
        last_time_sync_attempt = millis();
        time_synced = sync_time();
    }

    /* Once a minute, show that the device is alive and what its resources look like —
     * the same numbers that will ride along on every event as device context. */
    /* Not gated on sentry_buffered_count(): that only tracks the offline retry buffer —
     * see sentry_flush()'s own comment for why metrics and logs need this call regardless
     * of buffer state.
     *
     * On an interval, not every iteration: a transport can block for seconds when there is
     * no route (the serial relay waits for a host that may not be listening), so flushing
     * every pass would turn `loop()` into a chain of timeouts. */
    static uint32_t last_flush = 0;
    if (millis() - last_flush >= 30000) {
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
