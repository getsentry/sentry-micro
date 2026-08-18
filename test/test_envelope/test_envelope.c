/**
 * Host tests for the JSON writer and the envelope/event builder.
 *
 * Wire-format bugs are the expensive kind here: ingest answers `200` and silently drops a
 * malformed event, so on real hardware "it worked" and "it was thrown away" look identical
 * from the device. Checking the bytes on a host is the only cheap way to know.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "sentry_base64.h"
#include "sentry_envelope.h"
#include "sentry_json.h"

void setUp(void) { }
void tearDown(void) { }

/* ── JSON writer ──────────────────────────────────────────────────────────── */

static void test_writes_a_nested_document(void)
{
    char buf[256];
    sentry_json_t writer;
    sentry_json_init(&writer, buf, sizeof(buf));

    sentry_json_object_begin(&writer);
    sentry_json_kv_string(&writer, "a", "one");
    sentry_json_kv_uint(&writer, "b", 42);
    sentry_json_key(&writer, "c");
    sentry_json_object_begin(&writer);
    sentry_json_kv_bool(&writer, "d", true);
    sentry_json_object_end(&writer);
    sentry_json_key(&writer, "e");
    sentry_json_array_begin(&writer);
    sentry_json_uint(&writer, 1);
    sentry_json_uint(&writer, 2);
    sentry_json_array_end(&writer);
    sentry_json_object_end(&writer);

    TEST_ASSERT_TRUE(sentry_json_ok(&writer));
    /* Commas are the thing a hand-rolled writer gets wrong: after a value but not after a
     * key, and not before the first member of a fresh container. */
    TEST_ASSERT_EQUAL_STRING("{\"a\":\"one\",\"b\":42,\"c\":{\"d\":true},\"e\":[1,2]}", buf);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), writer.len);
}

static void test_escapes_json_strings(void)
{
    char buf[256];
    sentry_json_t writer;
    sentry_json_init(&writer, buf, sizeof(buf));

    sentry_json_object_begin(&writer);
    /* Split before "end" on purpose: C hex escapes are greedy, so "\x01end" is the
     * single byte 0x1E followed by "nd" — not 0x01 followed by "end". Ending the
     * string literal terminates the escape. (This test caught exactly that mistake
     * in its own first draft.) */
    sentry_json_kv_string(&writer, "k",
        "he said \"hi\"\\\n\ttab\x01"
        "end");
    sentry_json_object_end(&writer);

    TEST_ASSERT_TRUE(sentry_json_ok(&writer));
    /* 0x01 has no short escape, so it must be emitted as \\u0001 or the document is
     * invalid JSON — which ingest drops without telling the device. */
    TEST_ASSERT_EQUAL_STRING("{\"k\":\"he said \\\"hi\\\"\\\\\\n\\ttab\\u0001end\"}", buf);
}

static void test_passes_utf8_through_unchanged(void)
{
    char buf[64];
    sentry_json_t writer;
    sentry_json_init(&writer, buf, sizeof(buf));
    /* A real SSID from the bench: a curly apostrophe. Escaping bytes >= 0x80 would mangle
     * every multi-byte character. */
    sentry_json_string(&writer, "Joshua\xe2\x80\x99s iPhone");
    TEST_ASSERT_TRUE(sentry_json_ok(&writer));
    TEST_ASSERT_EQUAL_STRING("\"Joshua\xe2\x80\x99s iPhone\"", buf);
}

static void test_counts_without_a_buffer(void)
{
    char buf[256];
    sentry_json_t counting, writing;

    sentry_json_init(&counting, NULL, 0);
    sentry_json_init(&writing, buf, sizeof(buf));
    for (int i = 0; i < 2; i++) {
        sentry_json_t *w = i == 0 ? &counting : &writing;
        sentry_json_object_begin(w);
        sentry_json_kv_string(w, "key", "value\"escaped");
        sentry_json_kv_uint(w, "n", 1234567890);
        sentry_json_object_end(w);
    }

    /* The dry run must agree exactly with the real one, or the envelope item header will
     * declare a length that does not match its payload. */
    TEST_ASSERT_FALSE(counting.overflow);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), counting.len);
    TEST_ASSERT_EQUAL_size_t(writing.len, counting.len);
}

static void test_reports_overflow_and_refuses_partial_output(void)
{
    char buf[16];
    sentry_json_t writer;
    sentry_json_init(&writer, buf, sizeof(buf));

    sentry_json_object_begin(&writer);
    sentry_json_kv_string(&writer, "a-fairly-long-key", "and-a-long-value");
    sentry_json_object_end(&writer);

    TEST_ASSERT_FALSE(sentry_json_ok(&writer));
    /* len still reports what the document *would* have needed. */
    TEST_ASSERT_TRUE(writer.len > sizeof(buf));
}

/* ── event ids ────────────────────────────────────────────────────────────── */

static void test_formats_an_event_id(void)
{
    const uint8_t bytes[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
        0xbb, 0xcc, 0xdd, 0xee, 0xff };
    char id[SENTRY_MICRO_EVENT_ID_LEN];
    sentry_event_id_format(id, bytes);

    TEST_ASSERT_EQUAL_size_t(32, strlen(id));
    /* Byte 6 (0x66) is masked to version 4 -> 0x46; byte 8 (0x88) already carries the
     * 10xx variant so it survives; every other byte must appear verbatim. */
    TEST_ASSERT_EQUAL_STRING("00112233445546778899aabbccddeeff", id);
    TEST_ASSERT_EQUAL_CHAR('4', id[12]);
    TEST_ASSERT_TRUE(id[16] == '8' || id[16] == '9' || id[16] == 'a' || id[16] == 'b');
    for (size_t i = 0; i < 32; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            (id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'), id);
    }
}

/* ── events and envelopes ─────────────────────────────────────────────────── */

static sentry_device_info_t sample_device(void)
{
    sentry_device_info_t device;
    memset(&device, 0, sizeof(device));
    snprintf(device.chip_model, sizeof(device.chip_model), "ESP32");
    snprintf(device.device_id, sizeof(device.device_id), "142b2fa0ca8c");
    snprintf(device.sdk_version, sizeof(device.sdk_version), "v4.4.7-dirty");
    snprintf(device.arch, sizeof(device.arch), "xtensa");
    device.chip_revision = 1;
    device.cpu_cores = 2;
    device.flash_size_bytes = 4194304;
    device.total_heap_bytes = 286720;
    device.reset_reason = SENTRY_RESET_BROWNOUT;
    return device;
}

static sentry_event_t sample_event(const sentry_device_info_t *device)
{
    sentry_event_t event;
    memset(&event, 0, sizeof(event));
    event.event_id = "0123456789abcdef0123456789abcdef";
    event.level = SENTRY_LEVEL_FATAL;
    event.message = "Device rebooted: brownout";
    event.release = "sentry-micro-example@0.1.0";
    event.environment = "development";
    event.board = "m5stack-core";
    event.timestamp = 1755446400;
    event.device = device;
    event.free_heap_bytes = 255000;
    event.min_free_heap_bytes = 249856;
    event.uptime_ms = 61234;
    return event;
}

static void test_event_contains_the_fields_ingest_needs(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);

    char buf[2048];
    size_t len = sentry_event_write(buf, sizeof(buf), &event);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), len);

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"event_id\":\"0123456789abcdef0123456789abcdef\""));
    /* `native` is what routes the event into Sentry's native symbolication pipeline. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"platform\":\"native\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"level\":\"fatal\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755446400"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"release\":\"sentry-micro-example@0.1.0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"environment\":\"development\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"" SENTRY_MICRO_SDK_NAME "\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"formatted\":\"Device rebooted: brownout\""));

    /* The tags that make the issue stream useful. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"brownout\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"crashed\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"device_id\":\"142b2fa0ca8c\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"board\":\"m5stack-core\""));

    /* Documented context keys, so Sentry renders the device card rather than a blob. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"memory_size\":286720"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_memory\":255000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"arch\":\"xtensa\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"min_free_heap\":249856"));
}

static void test_event_omits_the_timestamp_when_the_clock_is_unset(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.timestamp = 0;

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    /* Absent, not zero: ingest substitutes its receive time only for a missing field, and
     * a device without SNTP would otherwise stamp every crash as 1970. */
    TEST_ASSERT_NULL(strstr(buf, "\"timestamp\""));
}

static void test_event_omits_unknown_optional_fields(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.board = NULL;
    event.release = NULL;
    event.message = "";

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NULL(strstr(buf, "\"board\""));
    TEST_ASSERT_NULL(strstr(buf, "\"release\""));
    TEST_ASSERT_NULL(strstr(buf, "\"message\""));
    /* And the document is still well-formed with those holes in it. */
    TEST_ASSERT_EQUAL_CHAR('{', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
}

static void test_envelope_header_length_matches_the_payload(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);

    char buf[4096];
    size_t len = sentry_envelope_write(buf, sizeof(buf), &event);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), len);

    /* Line 1: envelope header. Line 2: item header. Line 3: payload. */
    const char *line1_end = strchr(buf, '\n');
    TEST_ASSERT_NOT_NULL(line1_end);
    const char *line2_end = strchr(line1_end + 1, '\n');
    TEST_ASSERT_NOT_NULL(line2_end);

    TEST_ASSERT_EQUAL_STRING_LEN(
        "{\"event_id\":\"0123456789abcdef0123456789abcdef\"}", buf, line1_end - buf);

    /* The declared length must equal the actual payload bytes — if it does not, Relay
     * reads the wrong number of bytes and the whole envelope is discarded. */
    unsigned declared = 0;
    const char *length_field = strstr(line1_end, "\"length\":");
    TEST_ASSERT_NOT_NULL(length_field);
    TEST_ASSERT_EQUAL_INT(1, sscanf(length_field, "\"length\":%u", &declared));

    const char *payload = line2_end + 1;
    size_t actual = len - (size_t)(payload - buf) - 1; /* less the trailing newline */
    TEST_ASSERT_EQUAL_size_t(actual, declared);

    /* Payload is exactly the event document, and the envelope ends with a newline. */
    TEST_ASSERT_EQUAL_CHAR('{', payload[0]);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[len - 1]);
    TEST_ASSERT_EQUAL_CHAR('}', buf[len - 2]);
}

static void test_envelope_reports_the_size_it_needs(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);

    /* Dry run, then a real one that must agree. */
    size_t needed = sentry_envelope_write(NULL, 0, &event);
    TEST_ASSERT_TRUE(needed > 0);

    char buf[4096];
    TEST_ASSERT_EQUAL_size_t(needed, sentry_envelope_write(buf, sizeof(buf), &event));

    /* Too small: report the requirement, and emit nothing that could be mistaken for a
     * complete envelope. */
    char small[64];
    TEST_ASSERT_EQUAL_size_t(needed, sentry_envelope_write(small, sizeof(small), &event));
    TEST_ASSERT_EQUAL_STRING("", small);
}

static void test_rejects_an_event_without_an_id(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.event_id = NULL;

    char buf[512];
    TEST_ASSERT_EQUAL_size_t(0, sentry_envelope_write(buf, sizeof(buf), &event));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ── base64 ───────────────────────────────────────────────────────────────── */

static void test_encodes_base64_with_padding(void)
{
    char out[32];
    /* The RFC 4648 test vectors: each tail length exercises a different padding case. */
    TEST_ASSERT_EQUAL_size_t(0, sentry_base64_encode(out, sizeof(out), (const uint8_t *)"", 0));
    TEST_ASSERT_EQUAL_STRING("", out);
    sentry_base64_encode(out, sizeof(out), (const uint8_t *)"f", 1);
    TEST_ASSERT_EQUAL_STRING("Zg==", out);
    sentry_base64_encode(out, sizeof(out), (const uint8_t *)"fo", 2);
    TEST_ASSERT_EQUAL_STRING("Zm8=", out);
    sentry_base64_encode(out, sizeof(out), (const uint8_t *)"foo", 3);
    TEST_ASSERT_EQUAL_STRING("Zm9v", out);
    sentry_base64_encode(out, sizeof(out), (const uint8_t *)"foob", 4);
    TEST_ASSERT_EQUAL_STRING("Zm9vYg==", out);
    sentry_base64_encode(out, sizeof(out), (const uint8_t *)"fooba", 5);
    TEST_ASSERT_EQUAL_STRING("Zm9vYmE=", out);
    sentry_base64_encode(out, sizeof(out), (const uint8_t *)"foobar", 6);
    TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", out);
}

static void test_encodes_all_byte_values(void)
{
    /* An envelope is text today, but attachments will not be, and a sign-extension bug in
     * the >= 0x80 range would be invisible against ASCII-only test data. */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    char out[SENTRY_BASE64_ENCODED_LEN(256) + 1];
    size_t needed = sentry_base64_encode(out, sizeof(out), all, sizeof(all));
    TEST_ASSERT_EQUAL_size_t(344, needed);
    TEST_ASSERT_EQUAL_size_t(needed, strlen(out));
    TEST_ASSERT_EQUAL_STRING_LEN("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIj", out, 48);
    /* 256 bytes is 85 whole triples (bytes 0..254) plus a 1-byte tail, so the last group
     * encodes 0xFF alone: 0b111111 -> '/', 0b110000 -> 'w', then two pad characters. */
    TEST_ASSERT_EQUAL_STRING("/w==", out + 340);
}

static void test_base64_chunking_matches_a_single_pass(void)
{
    /* The serial transport encodes in 48-byte chunks to keep stack use constant. That is
     * only valid because 48 is a multiple of 3, so padding lands solely on the final
     * chunk; this asserts the property the transport depends on. */
    uint8_t data[200];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    char whole[SENTRY_BASE64_ENCODED_LEN(sizeof(data)) + 1];
    sentry_base64_encode(whole, sizeof(whole), data, sizeof(data));

    char chunked[SENTRY_BASE64_ENCODED_LEN(sizeof(data)) + 1];
    char piece[SENTRY_BASE64_ENCODED_LEN(48) + 1];
    chunked[0] = '\0';
    for (size_t offset = 0; offset < sizeof(data); offset += 48) {
        size_t take = sizeof(data) - offset;
        if (take > 48) {
            take = 48;
        }
        sentry_base64_encode(piece, sizeof(piece), data + offset, take);
        strcat(chunked, piece);
    }
    TEST_ASSERT_EQUAL_STRING(whole, chunked);
}

static void test_base64_refuses_a_buffer_it_cannot_fill(void)
{
    char small[4];
    /* Reports the requirement, writes nothing: a truncated base64 string decodes to a
     * truncated envelope, which is worse than sending none. */
    TEST_ASSERT_EQUAL_size_t(
        8, sentry_base64_encode(small, sizeof(small), (const uint8_t *)"foob", 4));
    TEST_ASSERT_EQUAL_STRING("", small);
}

/* ── debug ids ────────────────────────────────────────────────────────────── */

static void test_derives_the_debug_id_from_a_build_id(void)
{
    char debug_id[SENTRY_MICRO_DEBUG_ID_LEN];
    /* A real build-id taken off our own firmware, with the value Sentry indexes it under.
     * Cross-checked against Python's uuid.UUID(bytes_le=...), which is exactly what
     * getsentry/coredump-uploader uses. */
    TEST_ASSERT_TRUE(
        sentry_debug_id_from_code_id(debug_id, "4978b44454c7d2c87b9a58d5bfed23f3e1d4e831"));
    TEST_ASSERT_EQUAL_STRING("44b47849-c754-c8d2-7b9a-58d5bfed23f3", debug_id);

    /* The first three fields are byte-swapped and the last two are not. Reversing that
     * yields an id that looks perfectly valid and matches no uploaded file at all. */
    TEST_ASSERT_TRUE(sentry_debug_id_from_code_id(debug_id, "000102030405060708090a0b0c0d0e0f"));
    TEST_ASSERT_EQUAL_STRING("03020100-0504-0706-0809-0a0b0c0d0e0f", debug_id);
}

static void test_pads_a_short_build_id(void)
{
    char debug_id[SENTRY_MICRO_DEBUG_ID_LEN];
    /* Shorter than 16 bytes is zero-padded rather than rejected, matching the reference. */
    TEST_ASSERT_TRUE(sentry_debug_id_from_code_id(debug_id, "aabbccdd"));
    TEST_ASSERT_EQUAL_STRING("ddccbbaa-0000-0000-0000-000000000000", debug_id);
}

static void test_rejects_a_malformed_build_id(void)
{
    char debug_id[SENTRY_MICRO_DEBUG_ID_LEN];
    TEST_ASSERT_FALSE(sentry_debug_id_from_code_id(debug_id, "not-hex-at-all"));
    TEST_ASSERT_EQUAL_STRING("", debug_id);
    TEST_ASSERT_FALSE(sentry_debug_id_from_code_id(debug_id, ""));
    TEST_ASSERT_FALSE(sentry_debug_id_from_code_id(debug_id, NULL));
    /* An odd number of hex characters is not a whole number of bytes. */
    TEST_ASSERT_FALSE(sentry_debug_id_from_code_id(debug_id, "aabbc"));
}

static void test_event_carries_debug_meta_when_a_build_id_is_known(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.build_id = "4978b44454c7d2c87b9a58d5bfed23f3e1d4e831";
    event.image_addr = 0;

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"elf\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"code_id\":\"4978b44454c7d2c87b9a58d5bfed23f3e1d4e831\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"debug_id\":\"44b47849-c754-c8d2-7b9a-58d5bfed23f3\""));
    /* Addresses are strings in Sentry's schema, not JSON numbers. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"image_addr\":\"0x0\""));
}

/**
 * image_size is a number even though image_addr, right beside it, is a string.
 *
 * Relay types the two fields differently: image_addr is an Addr and takes "0x...", while
 * image_size is a plain unsigned integer and rejects a hex string. We shipped
 * `"image_size":"0xd759e3"` and Relay discarded the field on every event — visible only as
 * an "Event Processing Errors" note on the event page, because ingest still answers 200
 * and the issue still renders. Asserting the exact bytes is the only way this stays fixed.
 */
static void test_image_size_is_a_number_not_a_hex_string(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.build_id = "4978b44454c7d2c87b9a58d5bfed23f3e1d4e831";
    event.image_addr = 0x3f400020;
    event.image_size = 0xd759e3;

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"image_size\":14113251"));
    /* Belt and braces: no quoted form anywhere, in either notation. */
    TEST_ASSERT_NULL(strstr(buf, "\"image_size\":\""));
    /* And the neighbouring address is still a string, which is the confusing part. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"image_addr\":\"0x3f400020\""));
}

static void test_event_omits_debug_meta_without_a_build_id(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.build_id = NULL;

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    /* An empty images array would claim we know the build and cannot symbolicate it;
     * absent correctly says we do not know. */
    TEST_ASSERT_NULL(strstr(buf, "debug_meta"));

    /* And a malformed one must not emit a half-built block either. */
    event.build_id = "zzzz";
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NULL(strstr(buf, "debug_meta"));
}

/* ── crash events ─────────────────────────────────────────────────────────── */

static sentry_coredump_t sample_coredump(void)
{
    sentry_coredump_t crash;
    memset(&crash, 0, sizeof(crash));
    crash.available = true;
    snprintf(crash.task_name, sizeof(crash.task_name), "loopTask");
    snprintf(crash.exception_type, sizeof(crash.exception_type), "StoreProhibited");
    crash.exception_pc = 0x400d1234;
    /* A null dereference: the faulting address is 0, which must still be reported. */
    crash.exception_addr = 0x00000000;
    crash.exception_addr_valid = true;
    /* Innermost first, as ESP-IDF reports it. */
    crash.frames[0] = 0x400d1234;
    crash.frames[1] = 0x400d5678;
    crash.frames[2] = 0x400d9abc;
    crash.frame_count = 3;
    return crash;
}

/**
 * A crash report is tagged `crashed` even when the boot that sent it was not itself a crash.
 *
 * The core dump normally gets read on the boot right after the panic, so the reset reason
 * agrees. It comes apart when the dump outlives that boot — delivery failed and it waited in
 * flash, or the board was power-cycled or reflashed first. Keying the tag off the reset
 * reason alone produced a fatal crash event tagged `crashed: false`, seen in production on
 * an event whose dump survived a reflash. Any alert filtered on `crashed:true` would skip
 * exactly the crashes that were hardest to deliver.
 */
static void test_deferred_crash_is_still_tagged_as_a_crash(void)
{
    sentry_device_info_t device = sample_device();
    /* The reporting boot was an ordinary power-on; the crash happened some boots ago. */
    device.reset_reason = SENTRY_RESET_POWERON;
    sentry_coredump_t crash = sample_coredump();
    sentry_event_t event = sample_event(&device);
    event.coredump = &crash;

    char buf[3072];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"crashed\":true"));
    /* The reset reason still honestly describes the boot that did the reporting. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"poweron\""));
}

/** A plain boot report must not claim a crash just because the tag logic got clever. */
static void test_boot_event_without_a_coredump_is_not_a_crash(void)
{
    sentry_device_info_t device = sample_device();
    device.reset_reason = SENTRY_RESET_POWERON;
    sentry_event_t event = sample_event(&device);
    event.coredump = NULL;

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"crashed\":false"));
}

static void test_crash_event_carries_an_exception(void)
{
    sentry_device_info_t device = sample_device();
    sentry_coredump_t crash = sample_coredump();
    sentry_event_t event = sample_event(&device);
    event.coredump = &crash;

    char buf[3072];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"StoreProhibited\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"thread_id\":\"loopTask\""));
    /* The value line carries the faulting address, which is half the diagnosis for a null
     * dereference before any symbolication happens at all. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "accessing 0x00000000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"instruction_addr\":\"0x400d1234\""));
}

static void test_frames_are_reversed_for_sentry(void)
{
    sentry_device_info_t device = sample_device();
    sentry_coredump_t crash = sample_coredump();
    sentry_event_t event = sample_event(&device);
    event.coredump = &crash;

    char buf[3072];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);

    /* ESP-IDF gives innermost-first; Sentry renders oldest-first with the crash at the
     * bottom. So the outermost frame must appear before the crashing one in the JSON.
     * Getting this backwards produces a stacktrace that reads upside down and looks
     * entirely plausible, which is exactly why it is pinned here. */
    const char *outermost = strstr(buf, "\"instruction_addr\":\"0x400d9abc\"");
    const char *crashing = strstr(buf, "\"instruction_addr\":\"0x400d1234\"");
    TEST_ASSERT_NOT_NULL(outermost);
    TEST_ASSERT_NOT_NULL(crashing);
    TEST_ASSERT_TRUE_MESSAGE(outermost < crashing, "frames are not oldest-first");
}

static void test_truncated_backtraces_say_so(void)
{
    sentry_device_info_t device = sample_device();
    sentry_coredump_t crash = sample_coredump();
    sentry_event_t event = sample_event(&device);
    event.coredump = &crash;

    char buf[3072];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"backtrace\":\"complete\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"frames_captured\":3"));

    /* The RISC-V case: two frames, and the event must not let them pass for a whole stack.
     * Without this a reader concludes the crashing function was called directly by the one
     * above it, which on a C-series part is almost never true. */
    crash.frame_count = 2;
    crash.truncated = true;
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"backtrace\":\"truncated\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"backtrace_truncated\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"frames_captured\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"crash_task\":\"loopTask\""));
}

static void test_event_without_a_crash_has_no_exception(void)
{
    sentry_device_info_t device = sample_device();
    sentry_event_t event = sample_event(&device);
    event.coredump = NULL;

    char buf[2048];
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    /* An empty exception block would create an issue with no cause; absent is correct. */
    TEST_ASSERT_NULL(strstr(buf, "exception"));

    /* An unavailable coredump is the same as none. */
    sentry_coredump_t empty;
    memset(&empty, 0, sizeof(empty));
    event.coredump = &empty;
    TEST_ASSERT_TRUE(sentry_event_write(buf, sizeof(buf), &event) > 0);
    TEST_ASSERT_NULL(strstr(buf, "exception"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encodes_base64_with_padding);
    RUN_TEST(test_encodes_all_byte_values);
    RUN_TEST(test_base64_chunking_matches_a_single_pass);
    RUN_TEST(test_base64_refuses_a_buffer_it_cannot_fill);
    RUN_TEST(test_writes_a_nested_document);
    RUN_TEST(test_escapes_json_strings);
    RUN_TEST(test_passes_utf8_through_unchanged);
    RUN_TEST(test_counts_without_a_buffer);
    RUN_TEST(test_reports_overflow_and_refuses_partial_output);
    RUN_TEST(test_formats_an_event_id);
    RUN_TEST(test_derives_the_debug_id_from_a_build_id);
    RUN_TEST(test_pads_a_short_build_id);
    RUN_TEST(test_rejects_a_malformed_build_id);
    RUN_TEST(test_event_carries_debug_meta_when_a_build_id_is_known);
    RUN_TEST(test_image_size_is_a_number_not_a_hex_string);
    RUN_TEST(test_event_omits_debug_meta_without_a_build_id);
    RUN_TEST(test_event_contains_the_fields_ingest_needs);
    RUN_TEST(test_event_omits_the_timestamp_when_the_clock_is_unset);
    RUN_TEST(test_event_omits_unknown_optional_fields);
    RUN_TEST(test_deferred_crash_is_still_tagged_as_a_crash);
    RUN_TEST(test_boot_event_without_a_coredump_is_not_a_crash);
    RUN_TEST(test_crash_event_carries_an_exception);
    RUN_TEST(test_frames_are_reversed_for_sentry);
    RUN_TEST(test_truncated_backtraces_say_so);
    RUN_TEST(test_event_without_a_crash_has_no_exception);
    RUN_TEST(test_envelope_header_length_matches_the_payload);
    RUN_TEST(test_envelope_reports_the_size_it_needs);
    RUN_TEST(test_rejects_an_event_without_an_id);
    return UNITY_END();
}
