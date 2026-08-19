#include "sentry_metrics.h"

#include <stdio.h>
#include <string.h>

#include "sentry_json.h"

void sentry_metrics_reset(sentry_metrics_t *metrics)
{
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

/**
 * The slot for `name`, creating it if there is room.
 *
 * Compares by pointer first: names are string literals in every real use, so the common
 * case is a pointer equality test rather than a strcmp on the hot path this exists to make
 * measurable.
 */
static sentry_metric_t *slot_for(
    sentry_metrics_t *metrics, const char *name, sentry_metric_type_t type, const char *unit)
{
    sentry_metric_t *free_slot = NULL;
    for (uint8_t i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        sentry_metric_t *item = &metrics->items[i];
        if (!item->used) {
            if (!free_slot) {
                free_slot = item;
            }
            continue;
        }
        if (item->type == type && (item->name == name || strcmp(item->name, name) == 0)) {
            return item;
        }
    }
    if (!free_slot) {
        /* Full of other names. Dropping the new one beats evicting a total that is already
         * accumulating — a counter that silently restarts is worse than one that never
         * started, because only the second is visible. */
        metrics->dropped++;
        return NULL;
    }
    free_slot->name = name;
    free_slot->unit = unit;
    free_slot->type = type;
    free_slot->value = 0;
    free_slot->used = true;
    return free_slot;
}

bool sentry_metrics_count(
    sentry_metrics_t *metrics, const char *name, int64_t delta, const char *unit)
{
    if (!metrics || !name) {
        return false;
    }
    sentry_metric_t *item = slot_for(metrics, name, SENTRY_METRIC_COUNTER, unit);
    if (!item) {
        return false;
    }
    item->value += delta;
    return true;
}

bool sentry_metrics_gauge(
    sentry_metrics_t *metrics, const char *name, int64_t value, const char *unit)
{
    if (!metrics || !name) {
        return false;
    }
    sentry_metric_t *item = slot_for(metrics, name, SENTRY_METRIC_GAUGE, unit);
    if (!item) {
        return false;
    }
    /* Latest reading wins. A gauge is a sample, not a history — keeping every value would
     * need storage this deliberately does not have. */
    item->value = value;
    return true;
}

bool sentry_metrics_empty(const sentry_metrics_t *metrics)
{
    if (!metrics) {
        return true;
    }
    for (uint8_t i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        if (metrics->items[i].used) {
            return false;
        }
    }
    return true;
}

static const char *type_name(sentry_metric_type_t type)
{
    return type == SENTRY_METRIC_COUNTER ? "counter" : "gauge";
}

static uint8_t write_items(sentry_json_t *writer, const sentry_metrics_t *metrics,
    const char *trace_id, uint64_t now_unix_us)
{
    uint8_t count = 0;
    sentry_json_key(writer, "items");
    sentry_json_array_begin(writer);
    for (uint8_t i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        const sentry_metric_t *item = &metrics->items[i];
        if (!item->used) {
            continue;
        }
        sentry_json_object_begin(writer);
        sentry_json_kv_string(writer, "name", item->name);
        sentry_json_kv_string(writer, "type", type_name(item->type));
        sentry_json_key(writer, "value");
        sentry_json_int(writer, item->value);
        sentry_json_kv_string_opt(writer, "unit", item->unit);
        /* Required on every metric, and must come from the propagation context — the same
         * rule logs follow, and the reason both are resolved at flush rather than at the
         * call site. */
        sentry_json_kv_string(writer, "trace_id", trace_id);
        /* Required, despite being absent from the spec's own example payloads. Every metric
         * in a batch shares the flush time: an aggregate describes the interval that just
         * ended, and the individual observations inside it were never timestamped. */
        sentry_json_kv_micros(writer, "timestamp", now_unix_us);
        sentry_json_object_end(writer);
        count++;
    }
    sentry_json_array_end(writer);
    return count;
}

size_t sentry_metrics_envelope_write(char *buf, size_t cap, const sentry_metrics_t *metrics,
    const char *trace_id, uint64_t now_unix_us)
{
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
    if (!metrics || !trace_id || !trace_id[0] || now_unix_us == 0
        || sentry_metrics_empty(metrics)) {
        return 0;
    }

    /* Count first: the item header has to state both the payload length and how many
     * metrics are inside it, and neither is known until the payload has been formatted. */
    sentry_json_t counter;
    sentry_json_init(&counter, NULL, 0);
    sentry_json_object_begin(&counter);
    uint8_t item_count = write_items(&counter, metrics, trace_id, now_unix_us);
    sentry_json_object_end(&counter);
    size_t payload_len = counter.len;

    char header[160];
    int header_len = snprintf(header, sizeof(header),
        "{}\n{\"type\":\"trace_metric\",\"item_count\":%u,"
        "\"content_type\":\"application/vnd.sentry.items.trace-metric+json\","
        "\"length\":%u}\n",
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
    write_items(&writer, metrics, trace_id, now_unix_us);
    sentry_json_object_end(&writer);
    buf[header_len + payload_len] = '\n';
    buf[total] = '\0';
    return total;
}
