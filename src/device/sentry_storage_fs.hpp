/**
 * Filesystem-backed storage for the offline buffer.
 *
 * For firmware that already has a filesystem mounted — LittleFS, SPIFFS, an SD card — this
 * is the backend to use in preference to NVS. It reuses what is already there instead of
 * spending a second flash region on the same job, and a filesystem has far more room than
 * the stock 20 KB `nvs` partition, which matters if you buffer many events or attach
 * coredumps to them later.
 *
 *     #include <LittleFS.h>
 *     #include <device/sentry_storage_fs.hpp>
 *
 *     LittleFS.begin();                                  // your code, as it already is
 *     sentry_enable_buffering(sentry::storage_fs(LittleFS, 8));
 *
 * **It never mounts, formats, or unmounts.** The application owns the filesystem's
 * lifecycle, and a crash reporter that called `LittleFS.begin(true)` behind your back could
 * reformat a partition full of user data on the first boot after a mount hiccup. If the
 * filesystem is not mounted, buffering simply reports itself unavailable.
 *
 * Everything is confined to one directory (`/sentry` by default) so nothing here can
 * collide with application files.
 */
#ifndef SENTRY_MICRO_STORAGE_FS_HPP_INCLUDED
#define SENTRY_MICRO_STORAGE_FS_HPP_INCLUDED

#include "../core/sentry_buffer.h"

#if defined(ARDUINO)

#    include <FS.h>

namespace sentry {

/**
 * Storage for `slot_count` envelopes inside `dir` on an already-mounted `filesystem`.
 *
 * Takes `fs::FS`, the Arduino base class, so LittleFS, SPIFFS and SD all work — pass
 * whichever object your firmware already mounted.
 *
 * Returns NULL if `dir` cannot be created, which is the symptom of an unmounted or
 * read-only filesystem. The returned pointer has static lifetime; the SDK supports one
 * filesystem-backed buffer at a time, and `filesystem` must outlive it (the global
 * `LittleFS` object always does).
 */
const sentry_storage_t *storage_fs(
    fs::FS &filesystem, uint32_t slot_count, const char *dir = "/sentry");

} // namespace sentry

#endif /* ARDUINO */

#endif /* SENTRY_MICRO_STORAGE_FS_HPP_INCLUDED */
