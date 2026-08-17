#include "sentry_buffer.h"

#include <string.h>

/** True when the storage vtable has everything the policy below depends on. */
static bool storage_usable(const sentry_storage_t *storage)
{
    return storage && storage->write && storage->read && storage->erase && storage->load_meta
        && storage->save_meta && storage->slot_count > 0;
}

static void persist_meta(sentry_buffer_t *buffer)
{
    /* Best effort. Losing the metadata costs the buffered events on the next boot, which is
     * bad but survivable; refusing to buffer because metadata could not be written would
     * lose the event we are holding right now, which is worse. */
    buffer->storage->save_meta(buffer->storage->ctx, buffer->head, buffer->tail, buffer->dropped);
}

bool sentry_buffer_init(sentry_buffer_t *buffer, const sentry_storage_t *storage)
{
    if (!buffer) {
        return false;
    }
    memset(buffer, 0, sizeof(*buffer));
    if (!storage_usable(storage)) {
        return false;
    }
    buffer->storage = storage;

    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t dropped = 0;
    if (!storage->load_meta(storage->ctx, &head, &tail, &dropped)) {
        /* First run on a blank device: an empty buffer, not an error. */
        return true;
    }

    /* Metadata comes off flash that a half-finished write or a firmware downgrade could
     * have left inconsistent. Validate rather than trust: a head or tail past the end of
     * the ring would index out of bounds on the very first read. */
    if (head >= storage->slot_count || tail >= storage->slot_count) {
        return true;
    }

    buffer->head = head;
    buffer->tail = tail;
    buffer->dropped = dropped;
    /* head == tail is ambiguous between empty and full, and the distinction cannot be
     * recovered from the indices alone. Resolving it as "full" would replay stale events;
     * resolving it as "empty" loses at most a full buffer once. Count forward instead. */
    buffer->count = (tail >= head) ? (tail - head) : (storage->slot_count - head + tail);
    return true;
}

bool sentry_buffer_push(sentry_buffer_t *buffer, const uint8_t *envelope, size_t len)
{
    if (!buffer || !buffer->storage || !envelope || len == 0) {
        return false;
    }

    const sentry_storage_t *storage = buffer->storage;

    if (buffer->count == storage->slot_count) {
        /* Full: evict the oldest to make room. The erase is not checked, because failing to
         * erase must not stop us overwriting the slot anyway — the write below is what
         * actually reclaims it. */
        storage->erase(storage->ctx, buffer->head);
        buffer->head = (buffer->head + 1) % storage->slot_count;
        buffer->count--;
        if (buffer->dropped < UINT32_MAX) {
            buffer->dropped++;
        }
    }

    if (!storage->write(storage->ctx, buffer->tail, envelope, len)) {
        return false;
    }
    buffer->tail = (buffer->tail + 1) % storage->slot_count;
    buffer->count++;
    persist_meta(buffer);
    return true;
}

bool sentry_buffer_peek(sentry_buffer_t *buffer, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!buffer || !buffer->storage || buffer->count == 0 || !out || !out_len) {
        return false;
    }
    return buffer->storage->read(buffer->storage->ctx, buffer->head, out, cap, out_len);
}

bool sentry_buffer_pop(sentry_buffer_t *buffer)
{
    if (!buffer || !buffer->storage || buffer->count == 0) {
        return false;
    }
    const sentry_storage_t *storage = buffer->storage;
    storage->erase(storage->ctx, buffer->head);
    buffer->head = (buffer->head + 1) % storage->slot_count;
    buffer->count--;
    persist_meta(buffer);
    return true;
}

uint32_t sentry_buffer_count(const sentry_buffer_t *buffer) { return buffer ? buffer->count : 0; }

uint32_t sentry_buffer_dropped(const sentry_buffer_t *buffer)
{
    return buffer ? buffer->dropped : 0;
}

void sentry_buffer_reset_dropped(sentry_buffer_t *buffer)
{
    if (!buffer || !buffer->storage) {
        return;
    }
    buffer->dropped = 0;
    persist_meta(buffer);
}

void sentry_buffer_clear(sentry_buffer_t *buffer)
{
    if (!buffer || !buffer->storage) {
        return;
    }
    while (buffer->count > 0) {
        sentry_buffer_pop(buffer);
    }
}
