#include "sentry_envelope.h"

#include <stdio.h>
#include <string.h>

#include "sentry_json.h"

const char *sentry_level_name(sentry_level_t level)
{
    switch (level) {
    case SENTRY_LEVEL_DEBUG:
        return "debug";
    case SENTRY_LEVEL_INFO:
        return "info";
    case SENTRY_LEVEL_WARNING:
        return "warning";
    case SENTRY_LEVEL_FATAL:
        return "fatal";
    case SENTRY_LEVEL_ERROR:
    default:
        return "error";
    }
}

void sentry_event_id_format(char *out, const uint8_t random_bytes[16])
{
    static const char hex[] = "0123456789abcdef";

    uint8_t bytes[16];
    memcpy(bytes, random_bytes, sizeof(bytes));
    /* Stamp the RFC 4122 version (4) and variant (10xx) bits. Sentry accepts any 32 hex
     * digits, but emitting a well-formed v4 costs two masks and keeps the ids valid
     * anywhere else they get pasted. */
    bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);

    for (size_t i = 0; i < 16; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[32] = '\0';
}

/** Write the event document into `writer`, which may be in counting mode. */
static void write_event(sentry_json_t *writer, const sentry_event_t *event)
{
    const sentry_device_info_t *device = event->device;

    sentry_json_object_begin(writer);

    sentry_json_kv_string(writer, "event_id", event->event_id);
    /* `native` is what makes Sentry run the event through its native symbolication
     * pipeline — the same one that turns instruction addresses into function names for
     * C/C++ desktop crashes, and the reason an ESP32 backtrace can be symbolicated at all. */
    sentry_json_kv_string(writer, "platform", "native");
    sentry_json_kv_string(writer, "level", sentry_level_name(event->level));

    if (event->timestamp > 0) {
        /* Omitted entirely when the device has no clock, so ingest substitutes its receive
         * time. See sentry_device_unix_time() for why that is the honest default. */
        sentry_json_kv_uint(writer, "timestamp", event->timestamp);
    }

    sentry_json_kv_string_opt(writer, "release", event->release);
    sentry_json_kv_string_opt(writer, "environment", event->environment);

    /* Identifies the SDK to Sentry; shows up in the event's metadata and in SDK-adoption
     * dashboards, which is how "does anyone use sentry-micro" gets answered later. */
    sentry_json_key(writer, "sdk");
    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "name", SENTRY_MICRO_SDK_NAME);
    sentry_json_kv_string(writer, "version", SENTRY_MICRO_SDK_VERSION);
    sentry_json_object_end(writer);

    if (event->message && event->message[0]) {
        sentry_json_key(writer, "message");
        sentry_json_object_begin(writer);
        sentry_json_kv_string(writer, "formatted", event->message);
        sentry_json_object_end(writer);
    }

    /* Tags are what the issue stream groups and filters on, so this set is chosen to answer
     * the questions a maker actually asks: which board, which chip, and why did it reboot. */
    sentry_json_key(writer, "tags");
    sentry_json_object_begin(writer);
    if (device) {
        sentry_json_kv_string_opt(writer, "chip", device->chip_model);
        sentry_json_kv_string_opt(writer, "device_id", device->device_id);
        sentry_json_kv_string(
            writer, "reset_reason", sentry_reset_reason_name(device->reset_reason));
        sentry_json_kv_bool(writer, "crashed", sentry_reset_reason_is_crash(device->reset_reason));
    }
    sentry_json_kv_string_opt(writer, "board", event->board);
    sentry_json_object_end(writer);

    sentry_json_key(writer, "contexts");
    sentry_json_object_begin(writer);

    if (device) {
        /* Sentry renders a known set of `device` keys specially in the UI, so these use the
         * documented names (`model`, `memory_size`, `free_memory`) rather than invented
         * ones — otherwise they land in a generic key/value blob instead of the device card. */
        sentry_json_key(writer, "device");
        sentry_json_object_begin(writer);
        sentry_json_kv_string(writer, "type", "device");
        sentry_json_kv_string_opt(writer, "model", device->chip_model);
        sentry_json_kv_string_opt(writer, "arch", device->arch);
        sentry_json_kv_uint(writer, "memory_size", device->total_heap_bytes);
        sentry_json_kv_uint(writer, "free_memory", event->free_heap_bytes);
        sentry_json_kv_uint(writer, "processor_count", device->cpu_cores);
        sentry_json_kv_uint(writer, "storage_size", device->flash_size_bytes);
        sentry_json_object_end(writer);

        sentry_json_key(writer, "os");
        sentry_json_object_begin(writer);
        sentry_json_kv_string(writer, "type", "os");
        sentry_json_kv_string(writer, "name", "esp-idf");
        sentry_json_kv_string_opt(writer, "version", device->sdk_version);
        sentry_json_object_end(writer);
    }

    /* Not a standard context, so it goes under its own key rather than polluting `device`
     * with fields Sentry would not render. These are the numbers that actually predict an
     * ESP32 field failure: heap headroom trending down, and how long it survived. */
    sentry_json_key(writer, "esp32");
    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "type", "esp32");
    sentry_json_kv_uint(writer, "uptime_ms", event->uptime_ms);
    sentry_json_kv_uint(writer, "free_heap", event->free_heap_bytes);
    sentry_json_kv_uint(writer, "min_free_heap", event->min_free_heap_bytes);
    if (device) {
        sentry_json_kv_uint(writer, "chip_revision", device->chip_revision);
    }
    sentry_json_object_end(writer);

    sentry_json_object_end(writer); /* contexts */

    sentry_json_object_end(writer); /* event */
}

size_t sentry_event_write(char *buf, size_t cap, const sentry_event_t *event)
{
    if (!event || !event->event_id) {
        if (buf && cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }

    sentry_json_t writer;
    sentry_json_init(&writer, buf, cap);
    write_event(&writer, event);

    if (buf && writer.overflow) {
        /* Never hand back a truncated document that looks like JSON. */
        buf[0] = '\0';
    }
    return writer.len;
}

size_t sentry_envelope_write(char *buf, size_t cap, const sentry_event_t *event)
{
    if (!event || !event->event_id) {
        if (buf && cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }

    /* The item header has to state the payload length, and the payload comes after it — so
     * measure first with a counting pass. That costs one extra formatting pass and saves a
     * second buffer, which is the right trade when the buffer is the scarce thing. */
    size_t payload_len = sentry_event_write(NULL, 0, event);

    char header[128];
    int header_len = snprintf(header, sizeof(header),
        "{\"event_id\":\"%s\"}\n{\"type\":\"event\",\"length\":%u}\n", event->event_id,
        (unsigned)payload_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        if (buf && cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }

    /* header + payload + the newline that terminates the item. */
    size_t total = (size_t)header_len + payload_len + 1;

    if (!buf) {
        return total;
    }
    if (total + 1 > cap) {
        buf[0] = '\0';
        return total;
    }

    memcpy(buf, header, (size_t)header_len);
    /* Write the payload straight after the header; the length is already known to fit. */
    sentry_event_write(buf + header_len, cap - (size_t)header_len, event);
    buf[header_len + payload_len] = '\n';
    buf[total] = '\0';
    return total;
}
