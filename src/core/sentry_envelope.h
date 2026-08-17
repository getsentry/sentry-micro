/**
 * Event and envelope construction.
 *
 * This is the heart of "the device builds the envelope; the transport is a dumb pipe". What
 * comes out of here is the exact ndjson that `POST /api/<project>/envelope/` expects, so
 * anything that can move bytes to a URL can deliver it — WiFi, a phone over BLE, a serial
 * cable — with no Sentry knowledge of its own.
 *
 * Portable C: this file is what the host test suite exercises, because getting the wire
 * format wrong is otherwise only discoverable by flashing a board and watching ingest
 * silently drop events.
 */
#ifndef SENTRY_MICRO_ENVELOPE_H_INCLUDED
#define SENTRY_MICRO_ENVELOPE_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_coredump.h"
#include "sentry_device_info.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Length of an event id string: 32 hex characters plus the terminator. */
#define SENTRY_MICRO_EVENT_ID_LEN 33

/** Severity, matching Sentry's `level` vocabulary. */
typedef enum {
    SENTRY_LEVEL_DEBUG = 0,
    SENTRY_LEVEL_INFO,
    SENTRY_LEVEL_WARNING,
    SENTRY_LEVEL_ERROR,
    SENTRY_LEVEL_FATAL,
} sentry_level_t;

/** Wire string for a level, e.g. "fatal". Never NULL. */
const char *sentry_level_name(sentry_level_t level);

/**
 * Format 16 random bytes as a Sentry event id: 32 lowercase hex digits, no dashes.
 *
 * The UUID version and variant bits are set, so the result is a well-formed v4 UUID even
 * though Sentry only requires 32 hex characters. `out` must hold
 * `SENTRY_MICRO_EVENT_ID_LEN` bytes.
 */
void sentry_event_id_format(char *out, const uint8_t random_bytes[16]);

/** Length of a formatted debug id: 36 characters plus the terminator. */
#define SENTRY_MICRO_DEBUG_ID_LEN 37

/**
 * Derive Sentry's `debug_id` from a GNU build-id.
 *
 * This is the join between an event and the debug files uploaded for the build that
 * produced it. Sentry indexes uploaded ELFs by this value, so getting the byte order wrong
 * means every symbolication lookup silently misses.
 *
 * The first 16 bytes of the build-id are read as a *little-endian* UUID — the first three
 * fields byte-swapped, the last two not — which is what `symbolic` does and what
 * `getsentry/coredump-uploader` reproduces in `code_id_to_debug_id`. A build-id shorter
 * than 16 bytes is zero-padded, matching the same reference.
 *
 * `out` must hold `SENTRY_MICRO_DEBUG_ID_LEN` bytes. Returns false if `code_id_hex` is not
 * valid hex, in which case `out` is left empty.
 */
bool sentry_debug_id_from_code_id(char *out, const char *code_id_hex);

/** Everything that varies between one event and the next. */
typedef struct {
    /** 32 hex characters. Required — ingest rejects an event without one. */
    const char *event_id;

    sentry_level_t level;

    /** Human-readable summary, e.g. "Device rebooted: brownout". Optional. */
    const char *message;

    /** From `sentry_options_t`. Release must match the uploaded debug files. */
    const char *release;
    const char *environment;
    /** Free-form hardware identifier, attached as a tag. Optional. */
    const char *board;

    /** Seconds since the Unix epoch, or 0 to omit and let ingest use its receive time. */
    uint64_t timestamp;

    /** Per-boot facts. Required. */
    const sentry_device_info_t *device;

    /**
     * GNU build-id of this firmware, lowercase hex. Optional.
     *
     * When set, the event carries a `debug_meta` image so Sentry can match it against the
     * ELF uploaded for this build and turn instruction addresses into functions and lines.
     * Without it, addresses stay hex forever.
     */
    const char *build_id;

    /**
     * Crash details, when this event reports one. Optional.
     *
     * When present the event carries an `exception` with a stacktrace, which is what Sentry
     * symbolicates against the uploaded debug files.
     */
    const sentry_coredump_t *coredump;

    /**
     * Address the image is loaded at — the ELF's lowest `PT_LOAD` virtual address.
     *
     * Sentry computes `instruction_addr - image_addr` and looks the result up against
     * symbols normalised by the object's own load address, so this must match the ELF or
     * every frame resolves to `<unknown>`. Not 0: an ESP32 image does not start at 0, and
     * "no relocation happens" is not the same as "the image is based at zero".
     */
    uint64_t image_addr;

    /**
     * Bytes the image spans, from `image_addr` to the end of its last loadable segment.
     *
     * Sentry decides which module a frame belongs to by testing it against this range. On
     * ESP32 the span is large and sparse — flash rodata, DRAM, IRAM and flash text sit far
     * apart in the address map — but it is the honest extent of the object.
     */
    uint64_t image_size;

    /* Sampled at event time rather than at boot, so they describe the moment. */
    uint32_t free_heap_bytes;
    uint32_t min_free_heap_bytes;
    uint64_t uptime_ms;
} sentry_event_t;

/**
 * Write the event JSON.
 *
 * Pass `buf = NULL` to compute the length without writing — which is how the envelope
 * builder learns the payload size for the item header without needing a second buffer.
 *
 * Returns the number of bytes the document needs, excluding the NUL. If that is >= `cap`,
 * nothing usable was written; retry with a larger buffer.
 */
size_t sentry_event_write(char *buf, size_t cap, const sentry_event_t *event);

/**
 * Write a complete envelope wrapping one event:
 *
 *     {"event_id":"..."}\n
 *     {"type":"event","length":N}\n
 *     {...event...}\n
 *
 * Returns the number of bytes needed, excluding the NUL. If that is >= `cap`, nothing
 * usable was written.
 */
size_t sentry_envelope_write(char *buf, size_t cap, const sentry_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_ENVELOPE_H_INCLUDED */
