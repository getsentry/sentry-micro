/**
 * Device context — the fields auto-attached to every event.
 *
 * This is the cheap half of the value proposition: even before a single stack frame is
 * symbolicated, knowing *which chip, which firmware, which reset reason, how much heap
 * was left* turns "it randomly reboots" into a bucketed, groupable issue.
 *
 * Collected once at init (the immutable parts) plus on demand (heap, uptime, RSSI).
 */
#ifndef SENTRY_MICRO_DEVICE_H_INCLUDED
#define SENTRY_MICRO_DEVICE_H_INCLUDED

#include "../core/sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Why the chip last came up.
 *
 * Deliberately a sentry-micro enum rather than a raw `esp_reset_reason_t`: this is the
 * primary grouping tag on every event, so its string form is part of the wire contract
 * and must not drift when ESP-IDF renumbers its enum.
 */
typedef enum {
    SENTRY_RESET_UNKNOWN = 0,
    SENTRY_RESET_POWERON,
    SENTRY_RESET_EXTERNAL,
    SENTRY_RESET_SOFTWARE,
    SENTRY_RESET_PANIC,
    SENTRY_RESET_INT_WDT,
    SENTRY_RESET_TASK_WDT,
    SENTRY_RESET_WDT,
    SENTRY_RESET_DEEPSLEEP,
    SENTRY_RESET_BROWNOUT,
    SENTRY_RESET_SDIO,
    SENTRY_RESET_USB,
    SENTRY_RESET_JTAG,
} sentry_reset_reason_t;

/** Immutable per-boot device facts. Small enough to keep a copy of. */
typedef struct {
    /** e.g. "ESP32-S3". */
    char chip_model[16];
    /** Stable per-device id derived from the factory MAC, e.g. "a1b2c3d4e5f6". */
    char device_id[13];
    /** ESP-IDF version the firmware was built against, e.g. "5.3.1". */
    char sdk_version[24];
    /** Wafer revision as ESP-IDF 5 reports it: `major * 100 + minor` (301 == v3.1). */
    uint16_t chip_revision;
    uint8_t cpu_cores;
    uint32_t flash_size_bytes;
    /** Total heap at boot; the denominator for "how bad is this leak". */
    uint32_t total_heap_bytes;
    sentry_reset_reason_t reset_reason;
} sentry_device_info_t;

/**
 * Populate `out` with this boot's device facts.
 *
 * Safe to call from `setup()`; reads only chip/eFuse registers and never touches WiFi.
 */
void sentry_device_info_get(sentry_device_info_t *out);

/** Stable string for a reset reason. Never NULL — unknown reasons map to "unknown". */
const char *sentry_reset_reason_name(sentry_reset_reason_t reason);

/** True when the last boot followed a crash (panic, watchdog, or brownout). */
bool sentry_reset_reason_is_crash(sentry_reset_reason_t reason);

/** Current free heap in bytes, sampled at call time. */
uint32_t sentry_device_free_heap(void);

/** Smallest free-heap value seen since boot — the number that predicts OOM reboots. */
uint32_t sentry_device_min_free_heap(void);

/** Milliseconds since boot. */
uint64_t sentry_device_uptime_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_DEVICE_H_INCLUDED */
