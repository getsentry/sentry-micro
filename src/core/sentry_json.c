#include "sentry_json.h"

#include <stdio.h>
#include <string.h>

/**
 * Append `len` bytes.
 *
 * The counting mode (`buf == NULL`) still advances `len`, which is what makes a dry run
 * report the true document size. Once overflowed we keep counting but stop copying, so a
 * caller can also learn how big a buffer they *should* have passed.
 */
static void append(sentry_json_t *writer, const char *data, size_t len)
{
    if (writer->buf && !writer->overflow) {
        /* Reserve one byte for the NUL so the buffer is always a valid C string. */
        if (writer->len + len + 1 > writer->cap) {
            writer->overflow = true;
        } else {
            memcpy(writer->buf + writer->len, data, len);
            writer->buf[writer->len + len] = '\0';
        }
    }
    writer->len += len;
}

static void append_char(sentry_json_t *writer, char c) { append(writer, &c, 1); }

static void append_cstr(sentry_json_t *writer, const char *s) { append(writer, s, strlen(s)); }

/** Emit the separating comma if a sibling value was already written at this level. */
static void separate(sentry_json_t *writer)
{
    if (writer->need_comma) {
        append_char(writer, ',');
    }
}

void sentry_json_init(sentry_json_t *writer, char *buf, size_t cap)
{
    writer->buf = buf;
    writer->cap = cap;
    writer->len = 0;
    writer->overflow = false;
    writer->need_comma = false;
    if (buf && cap > 0) {
        buf[0] = '\0';
    } else if (buf) {
        /* A zero-capacity buffer cannot even hold the NUL. */
        writer->overflow = true;
    }
}

void sentry_json_object_begin(sentry_json_t *writer)
{
    separate(writer);
    append_char(writer, '{');
    /* First member of a fresh container must not be preceded by a comma. */
    writer->need_comma = false;
}

void sentry_json_object_end(sentry_json_t *writer)
{
    append_char(writer, '}');
    /* The container itself is now a completed sibling. */
    writer->need_comma = true;
}

void sentry_json_array_begin(sentry_json_t *writer)
{
    separate(writer);
    append_char(writer, '[');
    writer->need_comma = false;
}

void sentry_json_array_end(sentry_json_t *writer)
{
    append_char(writer, ']');
    writer->need_comma = true;
}

/** Write a JSON string literal, with the escaping RFC 8259 requires. */
static void write_escaped(sentry_json_t *writer, const char *value)
{
    append_char(writer, '"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
        case '"':
            append_cstr(writer, "\\\"");
            break;
        case '\\':
            append_cstr(writer, "\\\\");
            break;
        case '\b':
            append_cstr(writer, "\\b");
            break;
        case '\f':
            append_cstr(writer, "\\f");
            break;
        case '\n':
            append_cstr(writer, "\\n");
            break;
        case '\r':
            append_cstr(writer, "\\r");
            break;
        case '\t':
            append_cstr(writer, "\\t");
            break;
        default:
            if (*p < 0x20) {
                /* Any other control character has no short form and must be escaped, or
                 * the document is invalid JSON. Device strings pick these up from
                 * truncated flash reads more often than you would hope. */
                char unicode[7];
                snprintf(unicode, sizeof(unicode), "\\u%04x", (unsigned)*p);
                append_cstr(writer, unicode);
            } else {
                /* >= 0x80 passes through untouched, so UTF-8 survives byte-for-byte. */
                append_char(writer, (char)*p);
            }
            break;
        }
    }
    append_char(writer, '"');
}

void sentry_json_key(sentry_json_t *writer, const char *key)
{
    separate(writer);
    write_escaped(writer, key);
    append_char(writer, ':');
    /* The value that follows is not a sibling of the key. */
    writer->need_comma = false;
}

void sentry_json_string(sentry_json_t *writer, const char *value)
{
    if (!value) {
        sentry_json_null(writer);
        return;
    }
    separate(writer);
    write_escaped(writer, value);
    writer->need_comma = true;
}

void sentry_json_uint(sentry_json_t *writer, uint64_t value)
{
    separate(writer);
    char digits[24];
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    append_cstr(writer, digits);
    writer->need_comma = true;
}

void sentry_json_int(sentry_json_t *writer, int64_t value)
{
    separate(writer);
    char digits[24];
    snprintf(digits, sizeof(digits), "%lld", (long long)value);
    append_cstr(writer, digits);
    writer->need_comma = true;
}

void sentry_json_bool(sentry_json_t *writer, bool value)
{
    separate(writer);
    append_cstr(writer, value ? "true" : "false");
    writer->need_comma = true;
}

void sentry_json_null(sentry_json_t *writer)
{
    separate(writer);
    append_cstr(writer, "null");
    writer->need_comma = true;
}

void sentry_json_kv_string(sentry_json_t *writer, const char *key, const char *value)
{
    sentry_json_key(writer, key);
    sentry_json_string(writer, value);
}

void sentry_json_kv_uint(sentry_json_t *writer, const char *key, uint64_t value)
{
    sentry_json_key(writer, key);
    sentry_json_uint(writer, value);
}

void sentry_json_kv_bool(sentry_json_t *writer, const char *key, bool value)
{
    sentry_json_key(writer, key);
    sentry_json_bool(writer, value);
}

void sentry_json_kv_string_opt(sentry_json_t *writer, const char *key, const char *value)
{
    if (!value || value[0] == '\0') {
        return;
    }
    sentry_json_kv_string(writer, key, value);
}

bool sentry_json_ok(const sentry_json_t *writer) { return writer && !writer->overflow; }
