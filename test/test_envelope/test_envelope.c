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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_writes_a_nested_document);
    RUN_TEST(test_escapes_json_strings);
    RUN_TEST(test_passes_utf8_through_unchanged);
    RUN_TEST(test_counts_without_a_buffer);
    RUN_TEST(test_reports_overflow_and_refuses_partial_output);
    RUN_TEST(test_formats_an_event_id);
    RUN_TEST(test_event_contains_the_fields_ingest_needs);
    RUN_TEST(test_event_omits_the_timestamp_when_the_clock_is_unset);
    RUN_TEST(test_event_omits_unknown_optional_fields);
    RUN_TEST(test_envelope_header_length_matches_the_payload);
    RUN_TEST(test_envelope_reports_the_size_it_needs);
    RUN_TEST(test_rejects_an_event_without_an_id);
    return UNITY_END();
}
