/**
 * Crash details recovered from a stored core dump.
 *
 * This is the payload that turns "the device rebooted" into "the device died dereferencing
 * null in `render_frame`". ESP-IDF's panic handler writes a full ELF core dump to a flash
 * partition before resetting; on the next boot `esp_core_dump_get_summary()` distils it
 * down to the crashing task, the exception PC, and a short backtrace — small enough to send
 * as an ordinary event, without shipping the whole dump.
 *
 * In `core/` because it is data shape and wire contract: these fields become
 * `exception.values[].stacktrace.frames[]`. Reading them off the chip is chip-specific and
 * lives in `device/`.
 */
#ifndef SENTRY_MICRO_COREDUMP_H_INCLUDED
#define SENTRY_MICRO_COREDUMP_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ESP-IDF's summary carries at most 16 program counters. */
#define SENTRY_MICRO_MAX_FRAMES 16

typedef struct {
    /** False when no core dump was stored, or it failed its checksum. */
    bool available;

    /** FreeRTOS task that faulted, e.g. "loopTask". */
    char task_name[16];

    /**
     * Human-readable exception cause, e.g. "LoadProhibited". Empty when unknown.
     *
     * Becomes the issue title in Sentry, so it is a stable string decided by the SDK rather
     * than a raw architectural cause code that would mean nothing in the issue stream.
     */
    char exception_type[32];

    /** Program counter at the fault — the innermost frame. */
    uint32_t exception_pc;

    /**
     * Data address that caused the fault.
     *
     * Only meaningful for load/store exceptions, hence the separate validity flag rather
     * than treating 0 as "unknown": a null-pointer dereference faults at exactly address 0,
     * and that is the single most useful crash address there is.
     */
    uint32_t exception_addr;
    bool exception_addr_valid;

    /**
     * Program counters, innermost (crashing) frame first — the order ESP-IDF reports.
     * Sentry wants the opposite, and the envelope builder reverses them on the way out.
     */
    uint32_t frames[SENTRY_MICRO_MAX_FRAMES];
    uint32_t frame_count;

    /**
     * ESP-IDF flagged the unwind as incomplete or corrupt.
     *
     * Reported rather than hidden: a truncated backtrace is still useful, but a reader who
     * does not know it is truncated will draw the wrong conclusion from where it ends.
     */
    bool truncated;

    /** SHA-256 prefix of the crashing app's ELF, as ESP-IDF records it. A cross-check that
     *  the core dump really belongs to the firmware currently running. */
    char app_elf_sha256[33];
} sentry_coredump_t;

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_COREDUMP_H_INCLUDED */
