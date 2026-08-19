/**
 * Host tests for transactions and spans.
 *
 * The thing worth pinning here is that a duration is *correct*, and that the SDK refuses
 * to invent one. A transaction carries two timestamps whose difference is the entire
 * measurement, and unlike an event's timestamp there is no server-side substitute: the
 * server observes one moment, and a duration needs two. So a transaction anchored to
 * nothing would place real work at an arbitrary point in time and look exactly like one
 * that did not.
 *
 * The clock is a parameter, as everywhere else in `core/`, so a run of hours costs
 * microseconds here.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "sentry_envelope.h"
#include "sentry_span.h"

#define TRACE "d49d9bf66f13450b81f65bc51cf49c03"
#define PARENT "7c51afd529da4a2a"

/* 2026-08-19T00:00:00Z, in microseconds. */
#define NOW_US 1755561600000000ULL

static const uint8_t SPAN_BYTES[8] = { 0xbb, 0x8f, 0x27, 0x81, 0x30, 0x53, 0x5c, 0x3c };

void setUp(void) { }
void tearDown(void) { }

static sentry_trace_context_t a_trace(void)
{
    sentry_trace_context_t trace;
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&trace, TRACE "-" PARENT "-1", NULL, SPAN_BYTES));
    return trace;
}

static sentry_transaction_meta_t a_meta(sentry_device_info_t *device)
{
    memset(device, 0, sizeof(*device));
    snprintf(device->chip_model, sizeof(device->chip_model), "ESP32");
    snprintf(device->device_id, sizeof(device->device_id), "aabbccddeeff");

    sentry_transaction_meta_t meta;
    meta.event_id = "0123456789abcdef0123456789abcdef";
    meta.release = "fw@1.0.0";
    meta.environment = "production";
    meta.board = "chromabay";
    meta.device = device;
    return meta;
}

static void test_duration_comes_from_the_monotonic_clock(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;

    sentry_transaction_begin(&txn, &trace, "set-colour", "device.operation", 1000);
    /* 1.5 seconds of monotonic time. */
    TEST_ASSERT_TRUE(sentry_transaction_end_at(&txn, 1500000 + 1000, NOW_US));

    TEST_ASSERT_EQUAL_UINT64(NOW_US, txn.end_unix_us);
    TEST_ASSERT_EQUAL_UINT64(NOW_US - 1500000, sentry_transaction_start_unix_us(&txn));
}

static void test_a_device_with_no_clock_produces_no_transaction(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;
    sentry_device_info_t device;
    sentry_transaction_meta_t meta = a_meta(&device);
    char buf[2048];

    sentry_transaction_begin(&txn, &trace, "set-colour", NULL, 1000);
    /* 0 is what the clock reads until the application seeds it — over BLE from the
     * companion app, or from NTP where there is WiFi. Never the SDK's job. */
    TEST_ASSERT_FALSE(sentry_transaction_end_at(&txn, 500000, 0));

    /* Refused, not stamped at the epoch. An error in the same situation would still send. */
    TEST_ASSERT_EQUAL_UINT(0, sentry_transaction_envelope_write(buf, sizeof(buf), &txn, &meta));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_writes_a_transaction_envelope(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;
    sentry_device_info_t device;
    sentry_transaction_meta_t meta = a_meta(&device);
    char buf[3072];

    sentry_transaction_begin(&txn, &trace, "set-colour", "device.operation", 0);
    sentry_span_t *decode = sentry_span_open(&txn, "ble.decode", "0xa0be", SPAN_BYTES, 1000);
    sentry_span_close(decode, 21000);
    TEST_ASSERT_TRUE(sentry_transaction_end_at(&txn, 100000, NOW_US));

    size_t len = sentry_transaction_envelope_write(buf, sizeof(buf), &txn, &meta);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"transaction\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"transaction\":\"set-colour\""));
    /* Shares the caller's trace, so the device's work lands beside the app's. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" TRACE "\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"parent_span_id\":\"" PARENT "\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"op\":\"ble.decode\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"description\":\"0xa0be\""));
}

static void test_timestamps_are_decimal_seconds(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;
    sentry_device_info_t device;
    sentry_transaction_meta_t meta = a_meta(&device);
    char buf[3072];

    sentry_transaction_begin(&txn, &trace, "op", NULL, 0);
    TEST_ASSERT_TRUE(sentry_transaction_end_at(&txn, 1500000, NOW_US));
    TEST_ASSERT_TRUE(sentry_transaction_envelope_write(buf, sizeof(buf), &txn, &meta) > 0);

    /* Whole seconds would make most spans on a device zero-length, and the fraction must
     * be zero-padded or 1.000005 becomes 1.5. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561600.000000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"start_timestamp\":1755561598.500000"));
}

static void test_a_span_measurement_is_written_as_a_number(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;
    sentry_device_info_t device;
    sentry_transaction_meta_t meta = a_meta(&device);
    char buf[3072];

    sentry_transaction_begin(&txn, &trace, "op", NULL, 0);
    sentry_span_t *span = sentry_span_open(&txn, "render", NULL, SPAN_BYTES, 0);
    /* This is what metrics are now — Sentry aggregates numeric span attributes. */
    sentry_span_set_number(span, "free_heap", 201716);
    sentry_span_set_number(span, "rssi", -67);
    sentry_span_close(span, 16000);
    TEST_ASSERT_TRUE(sentry_transaction_end_at(&txn, 20000, NOW_US));
    TEST_ASSERT_TRUE(sentry_transaction_envelope_write(buf, sizeof(buf), &txn, &meta) > 0);

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_heap\":201716"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rssi\":-67"));
    /* A number, not a string — a quoted value would not aggregate. */
    TEST_ASSERT_NULL(strstr(buf, "\"free_heap\":\""));
}

static void test_setting_a_measurement_twice_replaces_it(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;

    sentry_transaction_begin(&txn, &trace, "op", NULL, 0);
    sentry_span_t *span = sentry_span_open(&txn, "render", NULL, SPAN_BYTES, 0);
    sentry_span_set_number(span, "free_heap", 1);
    sentry_span_set_number(span, "free_heap", 2);

    TEST_ASSERT_EQUAL_UINT(1, span->attr_count);
    TEST_ASSERT_EQUAL_INT64(2, span->attrs[0].value);
}

static void test_running_out_of_spans_is_reported_not_hidden(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;
    sentry_device_info_t device;
    sentry_transaction_meta_t meta = a_meta(&device);
    char buf[8192];

    sentry_transaction_begin(&txn, &trace, "op", NULL, 0);
    for (int i = 0; i < SENTRY_MICRO_MAX_SPANS + 3; i++) {
        sentry_span_t *span = sentry_span_open(&txn, "phase", NULL, SPAN_BYTES, (uint64_t)i);
        if (i >= SENTRY_MICRO_MAX_SPANS) {
            /* Full: NULL, and safe to close without checking. */
            TEST_ASSERT_NULL(span);
        }
        sentry_span_close(span, (uint64_t)i + 1);
    }
    TEST_ASSERT_EQUAL_UINT(SENTRY_MICRO_MAX_SPANS, txn.span_count);
    TEST_ASSERT_EQUAL_UINT(3, txn.dropped_spans);

    TEST_ASSERT_TRUE(sentry_transaction_end_at(&txn, 100000, NOW_US));
    TEST_ASSERT_TRUE(sentry_transaction_envelope_write(buf, sizeof(buf), &txn, &meta) > 0);
    /* Findable, not merely visible: a truncated trace otherwise reads as a complete
     * picture of a simpler operation than the one that ran. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"spans_dropped\":\"true\""));
}

static void test_an_unclosed_span_ends_with_the_transaction(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;

    sentry_transaction_begin(&txn, &trace, "op", NULL, 0);
    sentry_span_t *span = sentry_span_open(&txn, "forgotten", NULL, SPAN_BYTES, 1000);
    TEST_ASSERT_TRUE(sentry_transaction_end_at(&txn, 50000, NOW_US));

    /* Left open it would be written as zero-length at the start, which reads as work that
     * happened instantly rather than work nobody measured the end of. */
    TEST_ASSERT_TRUE(span->finished);
    TEST_ASSERT_EQUAL_UINT64(50000, span->end_uptime_us);
}

static void test_closing_a_span_twice_keeps_the_first_end(void)
{
    sentry_trace_context_t trace = a_trace();
    sentry_transaction_t txn;

    sentry_transaction_begin(&txn, &trace, "op", NULL, 0);
    sentry_span_t *span = sentry_span_open(&txn, "phase", NULL, SPAN_BYTES, 0);
    sentry_span_close(span, 1000);
    sentry_span_close(span, 9000);

    /* Stretching it to cover work it did not do would be the wrong kind of forgiving. */
    TEST_ASSERT_EQUAL_UINT64(1000, span->end_uptime_us);
}

static void test_null_is_safe_everywhere(void)
{
    /* A caller that never checks for NULL must still be correct — sentry_span_begin()
     * returns NULL whenever the transaction is full, which is not an error worth
     * propagating through firmware. */
    sentry_span_close(NULL, 1000);
    sentry_span_set_number(NULL, "x", 1);
    TEST_ASSERT_FALSE(sentry_transaction_end_at(NULL, 0, NOW_US));
    TEST_ASSERT_EQUAL_UINT64(0, sentry_transaction_start_unix_us(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_duration_comes_from_the_monotonic_clock);
    RUN_TEST(test_a_device_with_no_clock_produces_no_transaction);
    RUN_TEST(test_writes_a_transaction_envelope);
    RUN_TEST(test_timestamps_are_decimal_seconds);
    RUN_TEST(test_a_span_measurement_is_written_as_a_number);
    RUN_TEST(test_setting_a_measurement_twice_replaces_it);
    RUN_TEST(test_running_out_of_spans_is_reported_not_hidden);
    RUN_TEST(test_an_unclosed_span_ends_with_the_transaction);
    RUN_TEST(test_closing_a_span_twice_keeps_the_first_end);
    RUN_TEST(test_null_is_safe_everywhere);
    return UNITY_END();
}
