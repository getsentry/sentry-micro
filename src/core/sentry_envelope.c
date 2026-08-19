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
        /*
         * "This event describes a crash", not "this boot followed one".
         *
         * Usually the same thing: the core dump is read on the boot right after the panic,
         * so the reset reason is `panic` and both agree. They come apart when the dump
         * outlives that boot — delivery failed and it sat in flash, or the board was
         * reflashed or power-cycled before it could be sent. Then a real crash report
         * arrives from a `poweron` boot, and keying this off the reset reason alone tagged
         * it `crashed: false`. An alert filtered on `crashed:true` would silently skip
         * exactly the crashes that were hardest to deliver.
         *
         * `reset_reason` keeps describing the boot that did the reporting, because that is
         * what it is.
         */
        sentry_json_kv_bool(writer, "crashed",
            sentry_reset_reason_is_crash(device->reset_reason)
                || (event->coredump && event->coredump->available));
    }
    sentry_json_kv_string_opt(writer, "board", event->board);
    if (event->coredump && event->coredump->available) {
        /*
         * Whether the stack we sent is the whole stack.
         *
         * A tag rather than a buried field, because without it two frames look exactly like
         * a two-deep call stack and a reader will draw a confident wrong conclusion about
         * where the crash came from. It is also the dimension you want to filter on —
         * "show me the crashes we actually got a full trace for" — which on the C-series is
         * currently none of them: ESP-IDF does not unwind RISC-V.
         */
        sentry_json_kv_string(
            writer, "backtrace", event->coredump->truncated ? "truncated" : "complete");
    }
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

    /*
     * Not a standard context, so it goes under its own key rather than polluting `device`
     * with fields Sentry would not render. These are the numbers that actually predict an
     * ESP32 field failure: heap headroom trending down, and how long it survived.
     *
     * On a crash report they describe the boot that *sent* the report, not the one that
     * died — the core dump is read after the reboot, so uptime is typically a few hundred
     * milliseconds and the heap is whatever a fresh boot has. That is true of every crash
     * report, not some special case, so there is no flag to check: on an event with an
     * exception, read these as "the state when we told you", never "the state at the
     * crash". The crash's own state is in the stack trace.
     */
    sentry_json_key(writer, "esp32");
    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "type", "esp32");
    sentry_json_kv_uint(writer, "uptime_ms", event->uptime_ms);
    sentry_json_kv_uint(writer, "free_heap", event->free_heap_bytes);
    sentry_json_kv_uint(writer, "min_free_heap", event->min_free_heap_bytes);
    if (event->coredump && event->coredump->available) {
        sentry_json_kv_uint(writer, "frames_captured", event->coredump->frame_count);
        sentry_json_kv_bool(writer, "backtrace_truncated", event->coredump->truncated);
        sentry_json_kv_string_opt(writer, "crash_task", event->coredump->task_name);
    }
    if (device) {
        sentry_json_kv_uint(writer, "chip_revision", device->chip_revision);
    }
    sentry_json_object_end(writer);

    /*
     * What joins this event to everything else that was happening. `trace` is how Sentry
     * groups the device's crash with the app call that provoked it; `replay` is the direct
     * link to the recording of the person who made that call.
     *
     * Both are omitted entirely when no trace is active. An event with no trace is correct
     * and common — the device was idle — whereas an event carrying a stale trace would be
     * confidently wrong about which interaction caused it.
     */
    if (event->trace && event->trace->active) {
        sentry_json_key(writer, "trace");
        sentry_json_object_begin(writer);
        sentry_json_kv_string(writer, "type", "trace");
        sentry_json_kv_string(writer, "trace_id", event->trace->trace_id);
        sentry_json_kv_string(writer, "span_id", event->trace->span_id);
        if (event->trace->parent_span_id[0]) {
            sentry_json_kv_string(writer, "parent_span_id", event->trace->parent_span_id);
        }
        sentry_json_object_end(writer);

        if (event->trace->replay_id[0]) {
            sentry_json_key(writer, "replay");
            sentry_json_object_begin(writer);
            sentry_json_kv_string(writer, "type", "replay");
            sentry_json_kv_string(writer, "replay_id", event->trace->replay_id);
            sentry_json_object_end(writer);
        }
    }

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

        /*
         * Say only what the title does not. Sentry titles the issue from `type`, so
         * repeating it here spends the densest line in the UI on a duplicate, and the PC is
         * frame 0's `instruction_addr` — with debug files the reader gets a function and a
         * line number instead of the hex, and without them the hex is unusable anyway.
         *
         * Leaving it out also keeps `value` build-independent. Grouping normally keys off
         * the stack trace, but an event that carries no frames falls back to type + value,
         * and a PC in that string would split one real issue into one per firmware build.
         *
         * The faulting data address is the exception to all of that: on Load/StoreProhibited
         * `accessing 0x00000000` is the difference between a null dereference and a wild
         * pointer, and no amount of symbolication recovers it from the stack.
         *
         * Without one, the crashing task — never nothing. Omitting `value` was tried and
         * renders as the literal string "(No error message)" under the title, which is
         * worse than the duplication it was meant to avoid. The task name says something
         * the type does not ("which task died" being the next question after "what
         * happened"), and it is stable across builds, so it cannot split one issue per
         * firmware the way a PC would in the no-frames grouping fallback.
         */
        if (crash->exception_addr_valid) {
            char value[32];
            snprintf(value, sizeof(value), "accessing 0x%08x", (unsigned)crash->exception_addr);
            sentry_json_kv_string(writer, "value", value);
        } else if (crash->task_name[0]) {
            char value[sizeof("in ") + sizeof(crash->task_name)];
            snprintf(value, sizeof(value), "in %s", crash->task_name);
            sentry_json_kv_string(writer, "value", value);
        }
        sentry_json_kv_string_opt(writer, "thread_id", crash->task_name);
        /* `native` here too, so this exception goes through native symbolication. */
        sentry_json_kv_string(writer, "platform", "native");

        sentry_json_key(writer, "stacktrace");
        sentry_json_object_begin(writer);
        sentry_json_key(writer, "frames");
        sentry_json_array_begin(writer);
        /*
         * An unwinder that came back with nothing — a corrupted stack, or a RISC-V dump
         * ESP-IDF declined to walk — still leaves the PC, which *is* the crashing frame by
         * definition. Emit it as the one frame so it reaches symbolication and still names
         * the function that died; `value` no longer carries it, so without this the address
         * would be nowhere in the event. `frames_captured` stays at the unwinder's own
         * count of 0: this frame comes from the exception registers, not from a walk.
         */
        if (crash->frame_count == 0 && crash->exception_pc != 0) {
            char address[24];
            snprintf(address, sizeof(address), "0x%08x", (unsigned)crash->exception_pc);
            sentry_json_object_begin(writer);
            sentry_json_kv_string(writer, "instruction_addr", address);
            sentry_json_object_end(writer);
        }
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
            sentry_json_kv_string_opt(writer, "code_file", event->image_name);
            if (event->image_size > 0) {
                /*
                 * A plain number, unlike image_addr right above it. The two fields sit next
                 * to each other and look interchangeable, but Relay types them differently:
                 * image_addr is an Addr and accepts "0x...", image_size is a plain unsigned
                 * integer and a hex string is rejected outright.
                 *
                 * Sending "0xd759e3" here cost us every image_size we ever reported. The
                 * event still arrives, the issue still renders, and the only trace is an
                 * "Event Processing Errors" panel on the event page saying
                 * `Discarded invalid value ... expected an unsigned integer`. Nothing on
                 * the device sees it, because ingest still answers 200.
                 */
                sentry_json_kv_uint(writer, "image_size", event->image_size);
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

/** Write the transaction document into `writer`, which may be in counting mode. */
static void write_transaction(
    sentry_json_t *writer, const sentry_transaction_t *txn, const sentry_transaction_meta_t *meta)
{
    uint64_t start_us = sentry_transaction_start_unix_us(txn);

    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "event_id", meta->event_id);
    /* What makes this a transaction rather than an error; everything else is shared. */
    sentry_json_kv_string(writer, "type", "transaction");
    sentry_json_kv_string(writer, "platform", "native");
    sentry_json_kv_string(writer, "transaction", txn->name ? txn->name : "operation");
    sentry_json_kv_micros(writer, "start_timestamp", start_us);
    sentry_json_kv_micros(writer, "timestamp", txn->end_unix_us);
    sentry_json_kv_string_opt(writer, "release", meta->release);
    sentry_json_kv_string_opt(writer, "environment", meta->environment);

    sentry_json_key(writer, "sdk");
    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "name", SENTRY_MICRO_SDK_NAME);
    sentry_json_kv_string(writer, "version", SENTRY_MICRO_SDK_VERSION);
    sentry_json_object_end(writer);

    sentry_json_key(writer, "tags");
    sentry_json_object_begin(writer);
    if (meta->device) {
        sentry_json_kv_string_opt(writer, "chip", meta->device->chip_model);
        sentry_json_kv_string_opt(writer, "device_id", meta->device->device_id);
    }
    sentry_json_kv_string_opt(writer, "board", meta->board);
    if (txn->dropped_spans > 0) {
        /* Surfaced as a tag so a truncated trace can be *found*, not just noticed by
         * someone who happens to open it. A trace that quietly lost spans looks like a
         * complete picture of a simpler operation than the one that ran. */
        sentry_json_kv_string(writer, "spans_dropped", "true");
    }
    sentry_json_object_end(writer);

    sentry_json_key(writer, "contexts");
    sentry_json_object_begin(writer);
    sentry_json_key(writer, "trace");
    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "type", "trace");
    sentry_json_kv_string(writer, "trace_id", txn->trace.trace_id);
    sentry_json_kv_string(writer, "span_id", txn->trace.span_id);
    if (txn->trace.parent_span_id[0]) {
        sentry_json_kv_string(writer, "parent_span_id", txn->trace.parent_span_id);
    }
    sentry_json_kv_string_opt(writer, "op", txn->op);
    sentry_json_object_end(writer);
    sentry_json_object_end(writer);

    sentry_json_key(writer, "spans");
    sentry_json_array_begin(writer);
    for (uint8_t i = 0; i < txn->span_count; i++) {
        const sentry_span_t *span = &txn->spans[i];
        uint64_t span_start = start_us + (span->start_uptime_us - txn->start_uptime_us);
        uint64_t span_end = start_us + (span->end_uptime_us - txn->start_uptime_us);

        sentry_json_object_begin(writer);
        sentry_json_kv_string(writer, "span_id", span->span_id);
        /* Every span's parent is the transaction. A device operation is a flat list of
         * phases, and inventing a hierarchy the caller did not express would be fiction. */
        sentry_json_kv_string(writer, "parent_span_id", txn->trace.span_id);
        sentry_json_kv_string(writer, "trace_id", txn->trace.trace_id);
        sentry_json_kv_string_opt(writer, "op", span->op);
        sentry_json_kv_string_opt(writer, "description", span->description);
        sentry_json_kv_micros(writer, "start_timestamp", span_start);
        sentry_json_kv_micros(writer, "timestamp", span_end);
        if (span->attr_count > 0) {
            /* Numeric attributes are how metrics are reported now that the standalone
             * metrics API is gone; `data` is where Sentry aggregates them from. */
            sentry_json_key(writer, "data");
            sentry_json_object_begin(writer);
            for (uint8_t a = 0; a < span->attr_count; a++) {
                sentry_json_key(writer, span->attrs[a].key);
                sentry_json_int(writer, span->attrs[a].value);
            }
            sentry_json_object_end(writer);
        }
        sentry_json_object_end(writer);
    }
    sentry_json_array_end(writer);

    sentry_json_object_end(writer);
}

static size_t transaction_write(
    char *buf, size_t cap, const sentry_transaction_t *txn, const sentry_transaction_meta_t *meta)
{
    sentry_json_t writer;
    sentry_json_init(&writer, buf, cap);
    write_transaction(&writer, txn, meta);
    if (buf && writer.overflow) {
        /* Never hand back a truncated document that looks like JSON. */
        buf[0] = '\0';
    }
    return writer.len;
}

size_t sentry_transaction_envelope_write(
    char *buf, size_t cap, const sentry_transaction_t *txn, const sentry_transaction_meta_t *meta)
{
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
    /* No anchor means no honest timestamps; see sentry_transaction_end_at(). */
    if (!txn || !meta || !meta->event_id || txn->end_unix_us == 0
        || sentry_transaction_start_unix_us(txn) == 0) {
        return 0;
    }

    size_t payload_len = transaction_write(NULL, 0, txn, meta);

    char header[128];
    int header_len = snprintf(header, sizeof(header),
        "{\"event_id\":\"%s\"}\n{\"type\":\"transaction\",\"length\":%u}\n", meta->event_id,
        (unsigned)payload_len);
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
    transaction_write(buf + header_len, cap - (size_t)header_len, txn, meta);
    buf[header_len + payload_len] = '\n';
    buf[total] = '\0';
    return total;
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
