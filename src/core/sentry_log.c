#include "sentry_log.h"

#include <stdio.h>
#include <string.h>

#include "sentry_json.h"

void sentry_log_ring_reset(sentry_log_ring_t *ring)
{
    if (ring) {
        memset(ring, 0, sizeof(*ring));
    }
}

void sentry_log_ring_clear(sentry_log_ring_t *ring)
{
    if (!ring) {
        return;
    }
    /* Contents only. boot_seq and the anchors describe which boot is running and what time
     * it is — facts about the device, not about the batch just sent. A persistent ring that
     * forgot them here could not date the next line it recorded. */
    memset(ring->entries, 0, sizeof(ring->entries));
    ring->head = 0;
    ring->count = 0;
    ring->dropped = 0;
}

uint8_t sentry_log_ring_begin_boot(sentry_log_ring_t *ring)
{
    if (!ring) {
        return 0;
    }
    /* This boot's anchor becomes the one the surviving entries are dated against, because
     * it belonged to the boot that recorded them. Doing this before the counter advances is
     * what keeps the two consistent. */
    ring->prev_anchor_unix_us = ring->anchor_unix_us;
    ring->prev_anchor_uptime_us = ring->anchor_uptime_us;
    ring->anchor_unix_us = 0;
    ring->anchor_uptime_us = 0;
    ring->boot_seq++;
    return ring->count;
}

void sentry_log_ring_set_anchor(sentry_log_ring_t *ring, uint64_t unix_us, uint64_t uptime_us)
{
    if (!ring || unix_us == 0) {
        return;
    }
    ring->anchor_unix_us = unix_us;
    ring->anchor_uptime_us = uptime_us;
}

bool sentry_log_entry_is_recovered(const sentry_log_ring_t *ring, const sentry_log_entry_t *entry)
{
    return ring && entry && entry->boot_seq != ring->boot_seq;
}

uint32_t sentry_log_ring_layout_id(void)
{
    /* FNV-1a over everything that changes how these bytes are read back. Not a checksum of
     * the contents — a mismatch here means the *shape* is different, which is the failure a
     * magic word cannot see. */
    const uint32_t parts[] = {
        (uint32_t)SENTRY_MICRO_MAX_LOGS,
        (uint32_t)SENTRY_MICRO_LOG_BODY_LEN,
        (uint32_t)SENTRY_MICRO_TRACE_ID_LEN,
        (uint32_t)sizeof(sentry_log_entry_t),
        (uint32_t)sizeof(sentry_log_ring_t),
    };
    uint32_t id = 2166136261u;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        for (size_t b = 0; b < 4; b++) {
            id ^= (parts[i] >> (b * 8)) & 0xffu;
            id *= 16777619u;
        }
    }
    return id;
}

bool sentry_log_ring_push(sentry_log_ring_t *ring, sentry_level_t level, const char *trace_id,
    uint64_t uptime_us, const char *body, bool truncated)
{
    if (!ring || !body) {
        return false;
    }

    uint8_t index;
    bool evicted = false;
    if (ring->count == SENTRY_MICRO_MAX_LOGS) {
        /* Full: evict the oldest to make room. Unlike the metrics table, there is no
         * running total to protect here — the newest line is worth more than the one it
         * replaces, for a continuous stream. */
        index = ring->head;
        ring->head = (uint8_t)((ring->head + 1) % SENTRY_MICRO_MAX_LOGS);
        if (ring->dropped < UINT16_MAX) {
            ring->dropped++;
        }
        evicted = true;
    } else {
        index = (uint8_t)((ring->head + ring->count) % SENTRY_MICRO_MAX_LOGS);
        ring->count++;
    }

    sentry_log_entry_t *entry = &ring->entries[index];
    snprintf(entry->body, sizeof(entry->body), "%s", body);
    snprintf(entry->trace_id, sizeof(entry->trace_id), "%s", trace_id ? trace_id : "");
    entry->uptime_us = uptime_us;
    entry->level = level;
    /* ORs the caller's own knowledge with this call's own copy: a caller that already sized
     * `body` to fit (sentry_log()'s vsnprintf) contributes the true signal here, while one
     * that did not (calling this directly with an oversized string) is still caught by the
     * copy above having truncated it regardless of what `truncated` claimed. */
    entry->truncated = truncated || strlen(body) >= sizeof(entry->body);
    entry->used = true;
    entry->boot_seq = ring->boot_seq;
    return evicted;
}

bool sentry_log_ring_empty(const sentry_log_ring_t *ring) { return !ring || ring->count == 0; }

/** Sentry's log severity levels, mapping 1:1 onto sentry_level_t — only WARNING differs. */
static const char *log_level_name(sentry_level_t level)
{
    switch (level) {
    case SENTRY_LEVEL_DEBUG:
        return "debug";
    case SENTRY_LEVEL_INFO:
        return "info";
    case SENTRY_LEVEL_WARNING:
        return "warn";
    case SENTRY_LEVEL_ERROR:
        return "error";
    case SENTRY_LEVEL_FATAL:
        return "fatal";
    default:
        return "info";
    }
}

/** Lowest number in each level's range — see develop.sentry.dev/sdk/telemetry/logs. */
static int log_severity_number(sentry_level_t level)
{
    switch (level) {
    case SENTRY_LEVEL_DEBUG:
        return 5;
    case SENTRY_LEVEL_INFO:
        return 9;
    case SENTRY_LEVEL_WARNING:
        return 13;
    case SENTRY_LEVEL_ERROR:
        return 17;
    case SENTRY_LEVEL_FATAL:
        return 21;
    default:
        return 9;
    }
}

/**
 * This entry's wall-clock timestamp.
 *
 * For a line recorded by the boot that is flushing it, both clocks are read at the same
 * instant, so subtracting how long ago this entry's uptime was from that instant's uptime
 * gives how long ago it happened — the same derivation sentry_transaction_start_unix_us()
 * does for a span's start, applied per entry here instead of once.
 *
 * A line recovered from a *previous* boot cannot use that, and the failure is not a small
 * one. Uptime restarts at zero, so a recovered entry's uptime is large while this boot's is
 * tiny: the subtraction underflows, clamps, and every recovered line collapses onto the
 * flush instant. The whole pre-crash timeline — the reason for keeping these at all — would
 * arrive as a single stack of lines all stamped "now".
 *
 * So a recovered line is dated against the anchor belonging to the boot that recorded it,
 * carried across the reset by sentry_log_ring_begin_boot(). If that boot never learned the
 * date there is no anchor and no honest way to place the line, and it is stamped at this
 * boot's start instant instead: still wrong, but wrong in the one direction that cannot
 * mislead — the line really did happen before this boot began.
 */
static uint64_t entry_unix_us(const sentry_log_ring_t *ring, const sentry_log_entry_t *entry,
    uint64_t now_uptime_us, uint64_t now_unix_us)
{
    if (!sentry_log_entry_is_recovered(ring, entry)) {
        uint64_t elapsed = now_uptime_us > entry->uptime_us ? now_uptime_us - entry->uptime_us : 0;
        return elapsed > now_unix_us ? now_unix_us : now_unix_us - elapsed;
    }

    /* This boot began this long ago in wall-clock terms; nothing recovered can be newer. */
    uint64_t boot_instant = now_uptime_us > now_unix_us ? 0 : now_unix_us - now_uptime_us;

    if (ring->prev_anchor_unix_us == 0) {
        return boot_instant;
    }

    uint64_t when;
    if (entry->uptime_us >= ring->prev_anchor_uptime_us) {
        when = ring->prev_anchor_unix_us + (entry->uptime_us - ring->prev_anchor_uptime_us);
    } else {
        uint64_t before = ring->prev_anchor_uptime_us - entry->uptime_us;
        when = before > ring->prev_anchor_unix_us ? 0 : ring->prev_anchor_unix_us - before;
    }
    /* A recovered line cannot postdate the boot that came after it. An anchor read shortly
     * before a crash, against an entry recorded microseconds later, can round to just past
     * boot_instant; clamping keeps the ordering true rather than trusting the arithmetic. */
    return when > boot_instant ? boot_instant : when;
}

static uint8_t write_items(sentry_json_t *writer, const sentry_log_ring_t *ring,
    const char *fallback_trace_id, const char *device_id, uint64_t now_uptime_us,
    uint64_t now_unix_us)
{
    uint8_t count = 0;
    sentry_json_key(writer, "items");
    sentry_json_array_begin(writer);
    for (uint8_t i = 0; i < ring->count; i++) {
        const sentry_log_entry_t *entry = &ring->entries[(ring->head + i) % SENTRY_MICRO_MAX_LOGS];
        if (!entry->used) {
            continue;
        }
        sentry_json_object_begin(writer);
        sentry_json_kv_micros(
            writer, "timestamp", entry_unix_us(ring, entry, now_uptime_us, now_unix_us));
        /* Recorded trace wins; an idle-recorded line falls back to the one minted for this
         * batch, the same rule flush_metrics() already applies. */
        sentry_json_kv_string(
            writer, "trace_id", entry->trace_id[0] ? entry->trace_id : fallback_trace_id);
        sentry_json_kv_string(writer, "level", log_level_name(entry->level));
        sentry_json_kv_string(writer, "body", entry->body);
        sentry_json_key(writer, "severity_number");
        sentry_json_int(writer, log_severity_number(entry->level));

        /*
         * Attribute keys are abbreviated (`t7d`, `d_id`) rather than spelled out — an
         * envelope's worth of these is billed against SENTRY_MICRO_ENVELOPE_BUFFER_BYTES
         * per entry, and a full ring of maximum-length bodies is already close to that
         * budget before attributes are added at all.
         *
         * Deliberately *not* joined by a per-entry "this came from the previous boot" flag,
         * however useful that would read in the UI. The cheapest correct spelling of one
         * (`"pb":{"value":true,"type":"boolean"}`) is 34 bytes, and a full ring already
         * encodes to 1,944 of the 2,048 available — 17 bytes per entry of headroom. Six of
         * those would put a recovered ring over the budget, which is precisely the batch
         * that must never fail to send. The recovered timestamps, and the crash trace the
         * lines attach to, already carry the fact. See SDK-1420 for what happens when this
         * budget is estimated rather than measured.
         *
         * Both attributes are present only when they have
         * something to say: `t7d` only when true, `d_id` only when a device_id was given,
         * and `attributes` itself is skipped rather than written empty when neither applies.
         */
        bool has_device_id = device_id && device_id[0];
        if (entry->truncated || has_device_id) {
            sentry_json_key(writer, "attributes");
            sentry_json_object_begin(writer);
            if (entry->truncated) {
                sentry_json_key(writer, "t7d");
                sentry_json_object_begin(writer);
                sentry_json_kv_bool(writer, "value", entry->truncated);
                sentry_json_kv_string(writer, "type", "boolean");
                sentry_json_object_end(writer);
            }
            if (has_device_id) {
                sentry_json_key(writer, "d_id");
                sentry_json_object_begin(writer);
                sentry_json_kv_string(writer, "value", device_id);
                sentry_json_kv_string(writer, "type", "string");
                sentry_json_object_end(writer);
            }
            sentry_json_object_end(writer);
        }
        sentry_json_object_end(writer);
        count++;
    }
    sentry_json_array_end(writer);
    return count;
}

size_t sentry_log_envelope_write(char *buf, size_t cap, const sentry_log_ring_t *ring,
    const char *fallback_trace_id, const char *device_id, uint64_t now_uptime_us,
    uint64_t now_unix_us)
{
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
    if (!ring || !fallback_trace_id || !fallback_trace_id[0] || now_unix_us == 0
        || sentry_log_ring_empty(ring)) {
        return 0;
    }

    /* Count first, same reason as the metrics and event writers: the item header states
     * the payload length before the payload itself is known. */
    sentry_json_t counter;
    sentry_json_init(&counter, NULL, 0);
    sentry_json_object_begin(&counter);
    uint8_t item_count
        = write_items(&counter, ring, fallback_trace_id, device_id, now_uptime_us, now_unix_us);
    sentry_json_object_end(&counter);
    size_t payload_len = counter.len;

    char header[128];
    int header_len = snprintf(header, sizeof(header),
        "{}\n{\"type\":\"log\",\"item_count\":%u,"
        "\"content_type\":\"application/vnd.sentry.items.log+json\",\"length\":%u}\n",
        (unsigned)item_count, (unsigned)payload_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return 0;
    }

    size_t total = (size_t)header_len + payload_len + 1;
    if (!buf) {
        return total;
    }
    if (total + 1 > cap) {
        buf[0] = '\0';
        return total;
    }

    memcpy(buf, header, (size_t)header_len);
    sentry_json_t writer;
    sentry_json_init(&writer, buf + header_len, cap - (size_t)header_len);
    sentry_json_object_begin(&writer);
    write_items(&writer, ring, fallback_trace_id, device_id, now_uptime_us, now_unix_us);
    sentry_json_object_end(&writer);
    buf[header_len + payload_len] = '\n';
    buf[total] = '\0';
    return total;
}
