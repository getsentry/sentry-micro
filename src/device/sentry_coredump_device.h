/**
 * Reading the stored core dump off the chip.
 *
 * The shape of what comes back lives in `core/sentry_coredump.h`; this is the chip-specific
 * half, and the file a port to another MCU has to satisfy.
 */
#ifndef SENTRY_MICRO_COREDUMP_DEVICE_H_INCLUDED
#define SENTRY_MICRO_COREDUMP_DEVICE_H_INCLUDED

#include "../core/sentry_coredump.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether this build can capture core dumps at all.
 *
 * False when the firmware was built without `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, which is
 * worth surfacing: everything else still works, crashes just arrive without backtraces, and
 * that is otherwise indistinguishable from "no crashes have happened".
 */
bool sentry_coredump_is_supported(void);

/** Whether a valid core dump is stored right now. Verifies the image checksum. */
bool sentry_coredump_available(void);

/**
 * Read the stored core dump summary into `out`.
 *
 * Call early in boot, before anything overwrites the partition. Uses roughly 2.5 KB of the
 * calling task's stack for ESP-IDF's summary structure — comfortable inside Arduino's 8 KB
 * `loop` task, but worth knowing if you call it from a smaller one.
 *
 * Returns false when nothing is stored or the image fails its checksum.
 */
bool sentry_coredump_read(sentry_coredump_t *out);

/**
 * Erase the stored core dump.
 *
 * Call **only after the event has been delivered or safely buffered**. Erase too early and
 * a failed send loses the crash; erase never, and every subsequent boot re-reports the same
 * crash forever, which is precisely the boot-loop behaviour the SDK is meant to avoid.
 */
bool sentry_coredump_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_COREDUMP_DEVICE_H_INCLUDED */
