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
 * The same monotonic counter in microseconds.
 *
 * Spans measure their duration from this rather than from the wall clock, so the duration
 * is right even when the clock is set part way through an operation, or never. Only the
 * transaction's position on the timeline needs a real date.
 */
uint64_t sentry_device_uptime_us(void);

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
 * The same clock in microseconds, for anything that has to express a duration.
 *
 * Whole seconds are enough to say *when* an error happened — and if it is unknown, ingest
 * substitutes its receive time. Neither applies to a span: an operation runs for tens of
 * milliseconds to a couple of seconds, so at second resolution most of them are
 * zero-length, and a duration has no server-side substitute at all. The server observes
 * one moment; a duration needs two.
 *
 * Returns 0 when the clock has not been set, exactly like the seconds version. **Setting
 * it is the application's job, not the SDK's** — it needs a transport, a message format
 * and a drift policy, all of which belong to whoever built the device. ChromaBay seeds it
 * from the companion app over BLE and from NTP when WiFi is up; the SDK only reads it.
 */
uint64_t sentry_device_unix_time_us(void);

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

/**
 * Borrow a block of memory that survives a panic reset, to be written in place.
 *
 * The difference from `sentry_device_trace_persist()` is lifetime, not storage: a trace
 * context is copied in at known moments and can afford to be, whereas a log ring is written
 * on every recorded line and has to *live* here — a copy taken at intervals would lose
 * exactly the lines closest to the crash, which are the ones worth keeping.
 *
 * `layout_id` is what makes reading it back safe. The firmware that reads this block is not
 * necessarily the firmware that wrote it: an OTA can change a compile-time size and leave
 * the same bytes meaning something different, and a magic word alone would validate that
 * happily. A mismatch is treated exactly like no previous boot at all.
 *
 * A port with no memory that survives a reset should still return a block — an ordinary
 * static will do — and simply never report a recovery. That keeps the "where do logs live"
 * question entirely inside the device layer, so the portable core does not carry a second
 * whole-ring buffer in RAM for a case most ports do not have. NULL is reserved for `bytes`
 * being more than the port can supply at all, and turns logging off for the boot.
 *
 * `*recovered` is set true only when the block came back intact from a previous boot; when
 * false the block has been zeroed, so the caller can rely on it being clean rather than
 * holding whatever happened to be in that memory at power-on.
 */
void *sentry_device_persistent_block(size_t bytes, uint32_t layout_id, bool *recovered);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_DEVICE_H_INCLUDED */
