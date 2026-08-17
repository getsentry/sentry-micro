/**
 * Offline ring buffer for undelivered envelopes.
 *
 * This is what makes `SENTRY_SEND_UNAVAILABLE` mean "later" instead of "lost". It matters
 * far more here than on a desktop: an intermittently-connected device is the normal case,
 * not the exception, and the single most valuable event a crash reporter can produce — the
 * report of the crash that just happened — is generated at boot, *before* WiFi has
 * associated. Without somewhere to put it, that event is dropped every single time.
 *
 * Split the way the transport is: this file owns the portable policy (ordering, eviction,
 * accounting) and knows nothing about where the bytes live. Storage is a vtable, so the
 * same logic runs against NVS on an ESP32 and against plain memory in the host tests.
 */
#ifndef SENTRY_MICRO_BUFFER_H_INCLUDED
#define SENTRY_MICRO_BUFFER_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Where buffered envelopes are kept.
 *
 * Slots are addressed by index and are independent — the implementation is free to store
 * them as NVS keys, flash sectors, or array entries. Every call returns false on failure
 * rather than aborting: a full or worn-out flash must degrade into "cannot buffer", never
 * into a crash inside the crash reporter.
 */
typedef struct {
    /** Persist `len` bytes as slot `index`, replacing whatever was there. */
    bool (*write)(void *ctx, uint32_t index, const uint8_t *data, size_t len);
    /** Read slot `index` into `out`; sets `*out_len`. False if absent or too large. */
    bool (*read)(void *ctx, uint32_t index, uint8_t *out, size_t cap, size_t *out_len);
    /** Forget slot `index`. Erasing an empty slot is success, not failure. */
    bool (*erase)(void *ctx, uint32_t index);
    /** Load persisted head/tail/dropped. False when nothing has been stored yet. */
    bool (*load_meta)(void *ctx, uint32_t *head, uint32_t *tail, uint32_t *dropped);
    /** Persist head/tail/dropped. */
    bool (*save_meta)(void *ctx, uint32_t head, uint32_t tail, uint32_t dropped);

    void *ctx;
    /** Number of slots. The buffer holds at most this many envelopes. */
    uint32_t slot_count;
} sentry_storage_t;

typedef struct {
    const sentry_storage_t *storage;
    /** Index of the oldest envelope. */
    uint32_t head;
    /** Index one past the newest. Equal to head when empty (see `count`). */
    uint32_t tail;
    /** Envelopes currently stored. Tracked explicitly so full and empty are distinguishable. */
    uint32_t count;
    /**
     * Envelopes evicted because the buffer was full, since the counter was last reset.
     *
     * Kept rather than discarded silently so the fleet can eventually be told it is losing
     * events — a buffer that quietly overwrites looks identical to one that is working.
     */
    uint32_t dropped;
} sentry_buffer_t;

/**
 * Attach `buffer` to `storage` and restore any previously persisted state.
 *
 * Returns false if the storage is unusable, in which case buffering is disabled and the
 * SDK falls back to send-or-lose rather than refusing to run.
 */
bool sentry_buffer_init(sentry_buffer_t *buffer, const sentry_storage_t *storage);

/**
 * Store an envelope.
 *
 * When the buffer is full the **oldest** envelope is evicted. That direction is deliberate:
 * a device that keeps failing to deliver is usually still crashing, and the most recent
 * evidence describes the state it is actually in. It also bounds the damage from a boot
 * loop, which would otherwise fill the buffer with the first crash and then discard every
 * later one.
 *
 * Returns false only when the envelope could not be persisted at all.
 */
bool sentry_buffer_push(sentry_buffer_t *buffer, const uint8_t *envelope, size_t len);

/**
 * Copy the oldest envelope into `out` without removing it.
 *
 * Peek-then-pop rather than a single take, so a delivery that fails does not consume the
 * event — the whole point of the buffer is that a failed send is survivable.
 */
bool sentry_buffer_peek(sentry_buffer_t *buffer, uint8_t *out, size_t cap, size_t *out_len);

/** Discard the oldest envelope. Call only after it has actually been delivered. */
bool sentry_buffer_pop(sentry_buffer_t *buffer);

/** Number of envelopes waiting. */
uint32_t sentry_buffer_count(const sentry_buffer_t *buffer);

/** Envelopes evicted for lack of space since the last `sentry_buffer_reset_dropped()`. */
uint32_t sentry_buffer_dropped(const sentry_buffer_t *buffer);

/** Zero the dropped counter, once its value has been reported. */
void sentry_buffer_reset_dropped(sentry_buffer_t *buffer);

/** Discard everything. */
void sentry_buffer_clear(sentry_buffer_t *buffer);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_BUFFER_H_INCLUDED */
