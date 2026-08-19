#include "sentry_trace.h"

#include <string.h>

static const char HEX[] = "0123456789abcdef";

static void hex_encode(char *out, const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        out[i * 2] = HEX[bytes[i] >> 4];
        out[i * 2 + 1] = HEX[bytes[i] & 0x0f];
    }
    out[count * 2] = '\0';
}

/** Lowercase hex only. Sentry emits lowercase, and accepting mixed case here would let two
 *  spellings of the same id look like two different traces. */
static bool is_hex_run(const char *text, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        char c = text[i];
        bool digit = c >= '0' && c <= '9';
        bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

/** True when every byte of a hex id is '0' — the "no trace" sentinel some senders emit. */
static bool is_all_zero(const char *text, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (text[i] != '0') {
            return false;
        }
    }
    return true;
}

/** An org id is decimal digits only — same rule `sentry_dsn.c` applies to the DSN's own
 *  `o<digits>.` host prefix, so the two never disagree on what counts as one. */
static bool is_digit_run(const char *text, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

void sentry_trace_id_format(char *out, const uint8_t random_bytes[16])
{
    if (out && random_bytes) {
        hex_encode(out, random_bytes, 16);
    }
}

void sentry_span_id_format(char *out, const uint8_t random_bytes[8])
{
    if (out && random_bytes) {
        hex_encode(out, random_bytes, 8);
    }
}

/**
 * Pull one `sentry-`-prefixed value out of a baggage header.
 *
 * Baggage is `key=value,key=value`, and W3C allows whitespace around the separators.
 * Only the keys this SDK acts on are looked for; the rest of the Dynamic Sampling Context
 * is the caller's business and nothing here needs to understand it.
 */
static bool baggage_lookup(const char *baggage, const char *key, char *out, size_t out_cap)
{
    if (!baggage || !key || !out || out_cap == 0) {
        return false;
    }
    size_t key_len = strlen(key);

    for (const char *cursor = baggage; *cursor;) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        const char *entry_end = strchr(cursor, ',');
        if (!entry_end) {
            entry_end = cursor + strlen(cursor);
        }
        const char *equals = memchr(cursor, '=', (size_t)(entry_end - cursor));
        if (equals && (size_t)(equals - cursor) == key_len && strncmp(cursor, key, key_len) == 0) {
            const char *value = equals + 1;
            size_t len = (size_t)(entry_end - value);
            /* Trim trailing whitespace; a value longer than the buffer is dropped rather
             * than truncated, since half an id is worse than none. */
            while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
                len--;
            }
            if (len == 0 || len >= out_cap) {
                return false;
            }
            memcpy(out, value, len);
            out[len] = '\0';
            return true;
        }
        cursor = (*entry_end == ',') ? entry_end + 1 : entry_end;
    }
    return false;
}

bool sentry_trace_adopt_header(sentry_trace_context_t *ctx, const char *sentry_trace,
    const char *baggage, const uint8_t span_id_bytes[8])
{
    if (!ctx) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (!sentry_trace || !span_id_bytes) {
        return false;
    }

    /* <trace_id>-<span_id>[-<sampled>]: 32 hex, '-', 16 hex, optionally '-' and one flag. */
    size_t len = strlen(sentry_trace);
    if (len < 49 || sentry_trace[32] != '-') {
        return false;
    }
    if (!is_hex_run(sentry_trace, 32) || !is_hex_run(sentry_trace + 33, 16)) {
        return false;
    }
    /* An all-zero id is how some senders spell "no trace"; adopting it would put every
     * such device's events into one shared fictional trace. */
    if (is_all_zero(sentry_trace, 32) || is_all_zero(sentry_trace + 33, 16)) {
        return false;
    }

    bool sampled = false;
    bool decided = false;
    if (len > 49) {
        if (len != 51 || sentry_trace[49] != '-') {
            return false;
        }
        if (sentry_trace[50] == '1') {
            sampled = true;
        } else if (sentry_trace[50] != '0') {
            return false;
        }
        decided = true;
    }

    memcpy(ctx->trace_id, sentry_trace, 32);
    ctx->trace_id[32] = '\0';
    memcpy(ctx->parent_span_id, sentry_trace + 33, 16);
    ctx->parent_span_id[16] = '\0';
    sentry_span_id_format(ctx->span_id, span_id_bytes);
    ctx->sampled = sampled;
    ctx->has_sampling_decision = decided;
    ctx->active = true;

    /* Optional, and its absence is ordinary: only an app with replay running sends one.
     * A malformed value is discarded and the trace still stands — the replay link is a
     * bonus, not a reason to lose the trace. */
    char replay[SENTRY_MICRO_TRACE_ID_LEN];
    if (baggage_lookup(baggage, "sentry-replay_id", replay, sizeof(replay)) && strlen(replay) == 32
        && is_hex_run(replay, 32) && !is_all_zero(replay, 32)) {
        memcpy(ctx->replay_id, replay, sizeof(replay));
    }

    /* Same treatment: a caller not marking its org, or marking it with garbage, does not
     * cost the trace. `sentry_trace_can_continue()` treats a missing org_id as "nothing to
     * compare", not as a mismatch. */
    char org_id[SENTRY_MICRO_MAX_ORG_ID_LEN];
    if (baggage_lookup(baggage, "sentry-org_id", org_id, sizeof(org_id))) {
        size_t org_id_len = strlen(org_id);
        if (org_id_len > 0 && is_digit_run(org_id, org_id_len)) {
            memcpy(ctx->org_id, org_id, sizeof(org_id));
        }
    }
    return true;
}

bool sentry_trace_can_continue(const char *incoming_org_id, const char *own_org_id, bool strict)
{
    bool incoming_known = incoming_org_id && incoming_org_id[0];
    bool own_known = own_org_id && own_org_id[0];

    if (incoming_known && own_known) {
        return strcmp(incoming_org_id, own_org_id) == 0;
    }
    if (incoming_known != own_known) {
        return !strict;
    }
    /* Neither side knows its org id — nothing to disagree about. */
    return true;
}

void sentry_trace_begin(sentry_trace_context_t *ctx, const uint8_t trace_id_bytes[16],
    const uint8_t span_id_bytes[8], bool sampled)
{
    if (!ctx || !trace_id_bytes || !span_id_bytes) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    sentry_trace_id_format(ctx->trace_id, trace_id_bytes);
    sentry_span_id_format(ctx->span_id, span_id_bytes);
    ctx->sampled = sampled;
    ctx->has_sampling_decision = true;
    ctx->active = true;
}

void sentry_trace_clear(sentry_trace_context_t *ctx)
{
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

size_t sentry_trace_header_write(char *buf, size_t cap, const sentry_trace_context_t *ctx)
{
    if (!buf || !ctx || !ctx->active) {
        return 0;
    }
    /* 32 + 1 + 16 [+ 1 + 1] + NUL */
    size_t needed = ctx->has_sampling_decision ? 52 : 50;
    if (cap < needed) {
        return 0;
    }
    memcpy(buf, ctx->trace_id, 32);
    buf[32] = '-';
    memcpy(buf + 33, ctx->span_id, 16);
    size_t len = 49;
    if (ctx->has_sampling_decision) {
        buf[49] = '-';
        buf[50] = ctx->sampled ? '1' : '0';
        len = 51;
    }
    buf[len] = '\0';
    return len;
}
