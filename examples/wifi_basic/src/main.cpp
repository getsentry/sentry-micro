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

#include <sentry_micro.h>
#include <transport/sentry_transport_serial.hpp>
#include <transport/sentry_transport_wifi.hpp>

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

/* Identifies this build in Sentry, and is what debug files must be uploaded against. */
#define FIRMWARE_RELEASE "sentry-micro-example@" SENTRY_MICRO_VERSION

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
    /* show_hidden, and a longer dwell than the 300ms default. A beacon interval is ~100ms,
     * so a short dwell on a busy channel can miss an AP entirely — which reads as "the
     * network isn't there" when it is just quiet. Slow, but this only runs after a
     * failure, where being right matters more than being quick. */
    int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true, /*passive=*/false,
        /*max_ms_per_chan=*/1000);
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

    print_sentry_state();

    /*
     * Pick a route: WiFi if the device has one, otherwise relay through whatever is on the
     * other end of the USB cable (scripts/serial_relay.py). This is a hand-rolled version
     * of the auto-selection the SDK will eventually do itself — "WiFi if connected, else a
     * registered relay, else buffer to flash" — and it is what makes a board with no
     * usable network still testable.
     */
    if (connect_wifi()) {
        /* For production also call wifi_transport.set_ca_cert(); see the header for why
         * certificate verification is not on by default. */
        sentry::set_transport(wifi_transport);
        Serial.println("[sentry] transport: wifi");
    } else {
        sentry::set_transport(serial_transport);
        Serial.println("[sentry] transport: serial relay (run scripts/serial_relay.py)");
    }

    report_boot();
}

void loop()
{
    /* Once a minute, show that the device is alive and what its resources look like —
     * the same numbers that will ride along on every event as device context. */
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
