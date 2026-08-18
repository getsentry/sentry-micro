/**
 * Collecting the device context.
 *
 * The *shape* of that context — the struct, the reset-reason vocabulary — lives in
 * `core/sentry_device_info.h`, because it is wire contract and portable. This header is the
 * chip-specific half: the functions that go and read it off the silicon. Everything
 * declared here has a per-target implementation (`sentry_device_esp32.c` today), and this
 * is the file a port to ESP8266 or nRF52 has to satisfy.
 */
#ifndef SENTRY_MICRO_DEVICE_H_INCLUDED
#define SENTRY_MICRO_DEVICE_H_INCLUDED

#include "../core/sentry_device_info.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Populate `out` with this boot's device facts.
 *
 * Safe to call from `setup()`; reads only chip/eFuse registers and never touches WiFi.
 */
void sentry_device_info_get(sentry_device_info_t *out);

/** Current free heap in bytes, sampled at call time. */
uint32_t sentry_device_free_heap(void);

/** Smallest free-heap value seen since boot — the number that predicts OOM reboots. */
uint32_t sentry_device_min_free_heap(void);

/** Milliseconds since boot. */
uint64_t sentry_device_uptime_ms(void);

/**
 * Fill `out` with `len` cryptographically-usable random bytes.
 *
 * Used for event ids, which must not collide across a fleet — a boot-loop reporting the
 * same id would be deduplicated into a single event and hide its own frequency. Returns
 * false if no entropy source is available, in which case the caller must not pretend.
 */
bool sentry_device_random(uint8_t *out, size_t len);

/**
 * Seconds since the Unix epoch, or 0 when the clock has never been set.
 *
 * Zero is the normal state for a freshly-booted device: an ESP32 has no battery-backed
 * clock, so until SNTP has run it genuinely does not know the time. Reporting 0 lets the
 * event builder omit the timestamp and let ingest use its receive time, which is far better
 * than confidently stamping every crash as 1 January 1970.
 */
uint64_t sentry_device_unix_time(void);

/**
 * Keep `bytes` across a panic reboot, so a crash can be reported with the context it
 * happened inside.
 *
 * A crash is only reported on the *next* boot, so anything describing the moment it
 * occurred — which trace was being served, and whose replay it belonged to — has to
 * outlive the reset. On ESP32 that is RTC slow memory: cleared on power-on, preserved
 * across a software reset and a panic. Exactly the wanted semantics, since a cold boot had
 * no operation in flight to remember.
 *
 * Best-effort by definition. A port with nowhere to put it does nothing, and the crash is
 * then reported with no trace — which is the same answer an idle device gives, and correct
 * rather than merely tolerable.
 */
void sentry_device_trace_persist(const void *bytes, size_t len);

/**
 * Recover what `sentry_device_trace_persist()` stored, or zero `out` if there is nothing —
 * no previous boot, a power-on reset, or a port with no such storage.
 */
void sentry_device_trace_recover(void *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_DEVICE_H_INCLUDED */
