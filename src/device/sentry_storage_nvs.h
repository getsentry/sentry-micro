/**
 * NVS-backed storage for the offline buffer.
 *
 * NVS is ESP-IDF's wear-levelled key-value store in flash. It is the right home for this:
 * it survives reboots and OTA updates, it is already initialised by the Arduino core, and
 * it does not require the coredump or filesystem partitions to exist.
 *
 * Envelopes live under keys `sm_e0`, `sm_e1`, … and the ring indices under `sm_meta`, all
 * inside a private `sentry` namespace so nothing here can collide with application config.
 */
#ifndef SENTRY_MICRO_STORAGE_NVS_H_INCLUDED
#define SENTRY_MICRO_STORAGE_NVS_H_INCLUDED

#include "../core/sentry_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Storage for `slot_count` envelopes, or NULL if NVS is unavailable.
 *
 * The returned pointer has static lifetime. Sizing is a flash budget question: the stock
 * `nvs` partition is 20 KB, and a boot event is roughly 800 bytes, so eight slots is a
 * comfortable default and sixteen is about the practical ceiling before NVS runs out of
 * room for anything else. Exceeding what the partition can hold shows up as writes that
 * begin to fail, which the buffer reports rather than hides.
 *
 * Never erases NVS to recover space. The usual ESP-IDF idiom of erasing on
 * `ESP_ERR_NVS_NO_FREE_PAGES` would take the application's saved settings with it, and a
 * crash reporter destroying user data to report a crash is not a trade worth making — it
 * disables buffering instead.
 */
const sentry_storage_t *sentry_storage_nvs(uint32_t slot_count);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_STORAGE_NVS_H_INCLUDED */
