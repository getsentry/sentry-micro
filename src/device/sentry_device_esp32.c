#include "sentry_device.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h> /* gettimeofday */
#include <time.h>

#include "esp_attr.h" /* RTC_NOINIT_ATTR */
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

/*
 * Chip model is resolved at compile time from the IDF target macros rather than at
 * runtime from `esp_chip_info().model`. The enum gains a new constant with every new
 * silicon family, so a runtime switch either fails to build on older IDF or silently
 * reports "unknown" on newer chips; the CONFIG_IDF_TARGET_* macros have been stable
 * since IDF 4 and cost nothing at runtime.
 */
#if defined(CONFIG_IDF_TARGET_ESP32)
#    define SENTRY_CHIP_MODEL "ESP32"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#    define SENTRY_CHIP_MODEL "ESP32-S2"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#    define SENTRY_CHIP_MODEL "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32C2)
#    define SENTRY_CHIP_MODEL "ESP32-C2"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#    define SENTRY_CHIP_MODEL "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
#    define SENTRY_CHIP_MODEL "ESP32-C5"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#    define SENTRY_CHIP_MODEL "ESP32-C6"
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
#    define SENTRY_CHIP_MODEL "ESP32-H2"
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
#    define SENTRY_CHIP_MODEL "ESP32-P4"
#else
#    define SENTRY_CHIP_MODEL "ESP32-unknown"
#endif

/* Xtensa on the original line and the S-series; RISC-V on everything from the C-series on.
 * Compile-time for the same reason as the model: the answer is fixed by the target. */
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)                         \
    || defined(CONFIG_IDF_TARGET_ESP32S3)
#    define SENTRY_CHIP_ARCH "xtensa"
#else
#    define SENTRY_CHIP_ARCH "riscv"
#endif

static sentry_reset_reason_t map_reset_reason(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return SENTRY_RESET_POWERON;
    case ESP_RST_EXT:
        return SENTRY_RESET_EXTERNAL;
    case ESP_RST_SW:
        return SENTRY_RESET_SOFTWARE;
    case ESP_RST_PANIC:
        return SENTRY_RESET_PANIC;
    case ESP_RST_INT_WDT:
        return SENTRY_RESET_INT_WDT;
    case ESP_RST_TASK_WDT:
        return SENTRY_RESET_TASK_WDT;
    case ESP_RST_WDT:
        return SENTRY_RESET_WDT;
    case ESP_RST_DEEPSLEEP:
        return SENTRY_RESET_DEEPSLEEP;
    case ESP_RST_BROWNOUT:
        return SENTRY_RESET_BROWNOUT;
    case ESP_RST_SDIO:
        return SENTRY_RESET_SDIO;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    case ESP_RST_USB:
        return SENTRY_RESET_USB;
    case ESP_RST_JTAG:
        return SENTRY_RESET_JTAG;
#endif
    default:
        return SENTRY_RESET_UNKNOWN;
    }
}

void sentry_device_info_get(sentry_device_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    snprintf(out->chip_model, sizeof(out->chip_model), "%s", SENTRY_CHIP_MODEL);
    snprintf(out->arch, sizeof(out->arch), "%s", SENTRY_CHIP_ARCH);
    out->chip_revision = (uint16_t)chip.revision;
    out->cpu_cores = (uint8_t)chip.cores;

    snprintf(out->sdk_version, sizeof(out->sdk_version), "%s", esp_get_idf_version());

    /* The factory eFuse MAC is the only identifier that survives reflashing, NVS erase,
     * and a change of WiFi interface — so it is what the fleet is keyed on. Read from
     * eFuse directly rather than via WiFi so this works before (or without) any radio. */
    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(out->device_id, sizeof(out->device_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
            mac[2], mac[3], mac[4], mac[5]);
    }

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        out->flash_size_bytes = flash_size;
    }

    out->total_heap_bytes = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    out->reset_reason = map_reset_reason(esp_reset_reason());
}

uint32_t sentry_device_free_heap(void) { return esp_get_free_heap_size(); }

uint32_t sentry_device_min_free_heap(void) { return esp_get_minimum_free_heap_size(); }

uint64_t sentry_device_uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

bool sentry_device_random(uint8_t *out, size_t len)
{
    if (!out) {
        return false;
    }
    /* Hardware RNG. Note the ESP-IDF caveat: this is only truly random once RF (WiFi or
     * BT) has been enabled — before that it is seeded but not entropic. Good enough for
     * event ids, which need uniqueness across a fleet rather than unpredictability, and
     * the MAC-derived device id keeps two boards apart even in the degenerate case. */
    esp_fill_random(out, len);
    return true;
}

uint64_t sentry_device_uptime_us(void)
{
    /* esp_timer counts microseconds natively, so this is the raw value the millisecond
     * reading above is derived from rather than a scaled-up approximation. */
    return (uint64_t)esp_timer_get_time();
}

uint64_t sentry_device_unix_time(void)
{
    time_t now = time(NULL);
    /* An ESP32 has no battery-backed clock, so before SNTP runs `time()` returns something
     * near the epoch. Treat anything implausibly old as "unknown" rather than stamping
     * every field crash as 1970 — a wrong timestamp is worse than an absent one, because
     * ingest can substitute its receive time only when the field is missing.
     * 1600000000 is 2020-09-13; no real firmware build predates that. */
    if ((uint64_t)now < 1600000000ULL) {
        return 0;
    }
    return (uint64_t)now;
}

uint64_t sentry_device_unix_time_us(void)
{
    /* gettimeofday() rather than time(): same clock, microsecond resolution. The guard
     * below is the same one, applied to the same threshold, so the two cannot disagree
     * about whether the clock is trustworthy. */
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0 || (uint64_t)now.tv_sec < 1600000000ULL) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_usec;
}

/*
 * Trace context across a panic reboot.
 *
 * RTC slow memory is not cleared by the startup code on a software reset or a panic, only
 * on a true power-on. That is precisely the lifetime wanted here: a panic while serving a
 * request preserves that request's trace so the crash report can carry it, and a cold boot
 * forgets it because there was no request in flight to remember.
 *
 * The magic word is what distinguishes "we stored this" from whatever was in RTC RAM at
 * power-on. Without it, uninitialised memory would be read as a trace id and every event
 * would join a fictional trace.
 */
#define SENTRY_RTC_TRACE_MAGIC 0x53545243u /* "STRC" */
/* Headroom over sizeof(sentry_trace_context_t), not a tight fit: RTC slow memory is
 * plentiful enough on every target this SDK supports that a few dozen spare bytes cost
 * nothing, and it means the next field added to that struct does not also require touching
 * this file — only exceeding this cap does, and persisting a too-large context already
 * fails safe (see below). */
#define SENTRY_RTC_TRACE_CAP 160

RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    uint32_t len;
    uint8_t bytes[SENTRY_RTC_TRACE_CAP];
} g_rtc_trace;

void sentry_device_trace_persist(const void *bytes, size_t len)
{
    if (!bytes || len == 0 || len > sizeof(g_rtc_trace.bytes)) {
        /* Too large to keep is not an error worth failing a send over; the crash simply
         * arrives without a trace. Silently truncating would be worse — half a trace id
         * points at nothing while looking exactly like one that points somewhere. */
        g_rtc_trace.magic = 0;
        return;
    }
    memcpy(g_rtc_trace.bytes, bytes, len);
    g_rtc_trace.len = (uint32_t)len;
    g_rtc_trace.magic = SENTRY_RTC_TRACE_MAGIC;
}

void sentry_device_trace_recover(void *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    memset(out, 0, len);
    if (g_rtc_trace.magic == SENTRY_RTC_TRACE_MAGIC && g_rtc_trace.len == len) {
        memcpy(out, g_rtc_trace.bytes, len);
    }
}
