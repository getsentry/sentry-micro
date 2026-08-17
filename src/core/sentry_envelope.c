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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool sentry_debug_id_from_code_id(char *out, const char *code_id_hex)
{
    if (!out) {
        return false;
    }
    out[0] = '\0';
    if (!code_id_hex) {
        return false;
    }

    /* Decode up to 16 bytes, zero-padding a short build-id rather than rejecting it —
     * the same tolerance coredump-uploader has. */
    uint8_t bytes[16] = { 0 };
    size_t i = 0;
    for (; i < 16 && code_id_hex[i * 2] && code_id_hex[i * 2 + 1]; i++) {
        int high = hex_value(code_id_hex[i * 2]);
        int low = hex_value(code_id_hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[i] = (uint8_t)((high << 4) | low);
    }
    if (i == 0) {
        return false;
    }
    /* A trailing half-byte means the input was not whole hex bytes. */
    if (code_id_hex[i * 2] && !code_id_hex[i * 2 + 1]) {
        return false;
    }

    /* Little-endian UUID: the first three fields are byte-swapped, the last two are not.
     * Getting this backwards produces a plausible-looking id that matches nothing. */
    snprintf(out, SENTRY_MICRO_DEBUG_ID_LEN,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[3], bytes[2],
        bytes[1], bytes[0], bytes[5], bytes[4], bytes[7], bytes[6], bytes[8], bytes[9], bytes[10],
        bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return true;
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

    /* The crash itself. `exception` is what makes this an issue with a title and a
     * stacktrace rather than a log line with some numbers attached. */
    if (event->coredump && event->coredump->available) {
        const sentry_coredump_t *crash = event->coredump;

        sentry_json_key(writer, "exception");
        sentry_json_object_begin(writer);
        sentry_json_key(writer, "values");
        sentry_json_array_begin(writer);
        sentry_json_object_begin(writer);

        /* Groups the issue. Falls back to something honest rather than inventing a cause. */
        sentry_json_kv_string(
            writer, "type", crash->exception_type[0] ? crash->exception_type : "Panic");

        char value[96];
        if (crash->exception_addr_valid) {
            snprintf(value, sizeof(value), "%s at 0x%08x, accessing 0x%08x",
                crash->exception_type[0] ? crash->exception_type : "Panic",
                (unsigned)crash->exception_pc, (unsigned)crash->exception_addr);
        } else {
            snprintf(value, sizeof(value), "%s at 0x%08x",
                crash->exception_type[0] ? crash->exception_type : "Panic",
                (unsigned)crash->exception_pc);
        }
        sentry_json_kv_string(writer, "value", value);
        sentry_json_kv_string_opt(writer, "thread_id", crash->task_name);
        /* `native` here too, so this exception goes through native symbolication. */
        sentry_json_kv_string(writer, "platform", "native");

        sentry_json_key(writer, "stacktrace");
        sentry_json_object_begin(writer);
        sentry_json_key(writer, "frames");
        sentry_json_array_begin(writer);
        /*
         * Reversed. ESP-IDF reports the backtrace innermost-first (the crashing frame at
         * index 0); Sentry renders frames oldest-first, with the crash at the bottom. Emit
         * them in the wrong order and every stacktrace reads upside down — which looks
         * plausible enough that nobody notices immediately.
         */
        for (uint32_t i = crash->frame_count; i > 0; i--) {
            char address[24];
            snprintf(address, sizeof(address), "0x%08x", (unsigned)crash->frames[i - 1]);
            sentry_json_object_begin(writer);
            sentry_json_kv_string(writer, "instruction_addr", address);
            sentry_json_object_end(writer);
        }
        sentry_json_array_end(writer);
        sentry_json_object_end(writer); /* stacktrace */

        sentry_json_object_end(writer);
        sentry_json_array_end(writer);
        sentry_json_object_end(writer); /* exception */
    }

    /* The join to the uploaded debug files. Without this block Sentry has no way to know
     * which build the addresses in this event came from, and they stay hex forever. */
    if (event->build_id && event->build_id[0]) {
        char debug_id[SENTRY_MICRO_DEBUG_ID_LEN];
        if (sentry_debug_id_from_code_id(debug_id, event->build_id)) {
            sentry_json_key(writer, "debug_meta");
            sentry_json_object_begin(writer);
            sentry_json_key(writer, "images");
            sentry_json_array_begin(writer);
            sentry_json_object_begin(writer);
            sentry_json_kv_string(writer, "type", "elf");
            sentry_json_kv_string(writer, "code_id", event->build_id);
            sentry_json_kv_string(writer, "debug_id", debug_id);
            /* Hex string, not a number: Sentry's schema expects addresses as strings. */
            char image_addr[24];
            snprintf(
                image_addr, sizeof(image_addr), "0x%llx", (unsigned long long)event->image_addr);
            sentry_json_kv_string(writer, "image_addr", image_addr);
            if (event->image_size > 0) {
                char image_size[24];
                snprintf(image_size, sizeof(image_size), "0x%llx",
                    (unsigned long long)event->image_size);
                sentry_json_kv_string(writer, "image_size", image_size);
            }
            sentry_json_object_end(writer);
            sentry_json_array_end(writer);
            sentry_json_object_end(writer);
        }
    }

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
