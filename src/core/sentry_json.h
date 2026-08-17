/**
 * A fixed-buffer JSON writer.
 *
 * Sentry events are JSON, and a microcontroller has no room for a DOM: sentry-native builds
 * a `sentry_value_t` tree and serialises it, which costs an allocation per node. Here the
 * event is written straight into a caller-owned buffer in one pass, so the cost of an event
 * is exactly the bytes it occupies and nothing else.
 *
 * The writer never fails loudly. Overrunning the buffer sets a flag and stops writing, so
 * the caller checks `sentry_json_ok()` once at the end rather than after every field —
 * partial JSON is never emitted as if it were complete.
 *
 * Pass a NULL buffer to *count* instead of write. That makes the length of a document
 * computable without a second buffer, which is how the envelope builder learns the payload
 * size it has to put in the item header before it writes the payload itself.
 */
#ifndef SENTRY_MICRO_JSON_H_INCLUDED
#define SENTRY_MICRO_JSON_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Destination, or NULL to count bytes without writing them. */
    char *buf;
    /** Capacity of `buf` including room for the terminating NUL. */
    size_t cap;
    /** Bytes of JSON produced so far, excluding the NUL. */
    size_t len;
    /** Set once anything did not fit. The document is then incomplete and unusable. */
    bool overflow;
    /** Whether the next value at this nesting level needs a leading comma. */
    bool need_comma;
} sentry_json_t;

/** Begin writing into `buf` (or counting, if `buf` is NULL). */
void sentry_json_init(sentry_json_t *writer, char *buf, size_t cap);

void sentry_json_object_begin(sentry_json_t *writer);
void sentry_json_object_end(sentry_json_t *writer);
void sentry_json_array_begin(sentry_json_t *writer);
void sentry_json_array_end(sentry_json_t *writer);

/** Write an object key. The next call writes its value. */
void sentry_json_key(sentry_json_t *writer, const char *key);

/** Write a string value, escaped. A NULL `value` writes `null`. */
void sentry_json_string(sentry_json_t *writer, const char *value);
void sentry_json_uint(sentry_json_t *writer, uint64_t value);
void sentry_json_int(sentry_json_t *writer, int64_t value);
void sentry_json_bool(sentry_json_t *writer, bool value);
void sentry_json_null(sentry_json_t *writer);

/* Key + value in one call — most of an event is these. */
void sentry_json_kv_string(sentry_json_t *writer, const char *key, const char *value);
void sentry_json_kv_uint(sentry_json_t *writer, const char *key, uint64_t value);
void sentry_json_kv_bool(sentry_json_t *writer, const char *key, bool value);

/**
 * Key + value, skipped entirely when `value` is NULL or empty.
 *
 * Sentry treats an absent field and an empty one differently in the UI, and a device has
 * plenty of fields it may legitimately know nothing about (no board configured, no org id
 * on a self-hosted DSN). Omitting beats sending `""`.
 */
void sentry_json_kv_string_opt(sentry_json_t *writer, const char *key, const char *value);

/** True while the document is complete and well-formed. */
bool sentry_json_ok(const sentry_json_t *writer);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_JSON_H_INCLUDED */
