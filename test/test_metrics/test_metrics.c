/**
 * Host tests for Application Metrics.
 *
 * The property this exists for is that recording is cheap and does not send — a counter has
 * to be safe to call from a render loop, which is the one part of a device nobody can
 * currently measure. So most of what is pinned here is aggregation in place: a name hit a
 * thousand times occupies one slot and produces one number.
 *
 * The other half is what happens when the table fills, because the wrong choice there loses
 * a running total silently.
 */

#include <string.h>
#include <unity.h>

#include "sentry_metrics.h"

#define TRACE "d49d9bf66f13450b81f65bc51cf49c03"

void setUp(void) { }
void tearDown(void) { }

static void test_a_counter_accumulates_in_one_slot(void)
{
    sentry_metrics_t m;
    sentry_metrics_reset(&m);

    /* The render-loop case: many calls, one slot, no send. */
    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_TRUE(sentry_metrics_count(&m, "frames", 1, NULL));
    }
    TEST_ASSERT_EQUAL_INT64(1000, m.items[0].value);
    TEST_ASSERT_EQUAL_UINT(0, m.dropped);
    /* Only the first slot was ever used. */
    TEST_ASSERT_FALSE(m.items[1].used);
}

static void test_a_gauge_keeps_the_latest_reading(void)
{
    sentry_metrics_t m;
    sentry_metrics_reset(&m);

    sentry_metrics_gauge(&m, "free_heap", 201716, "byte");
    sentry_metrics_gauge(&m, "free_heap", 198004, "byte");

    /* A sample, not a history: keeping every value needs storage a device does not have. */
    TEST_ASSERT_EQUAL_INT64(198004, m.items[0].value);
    TEST_ASSERT_FALSE(m.items[1].used);
}

static void test_a_counter_and_a_gauge_of_the_same_name_are_different_metrics(void)
{
    sentry_metrics_t m;
    sentry_metrics_reset(&m);

    sentry_metrics_count(&m, "x", 5, NULL);
    sentry_metrics_gauge(&m, "x", 9, NULL);

    /* Summing a gauge into a counter would produce a number that means nothing. */
    TEST_ASSERT_TRUE(m.items[0].used && m.items[1].used);
    TEST_ASSERT_EQUAL_INT64(5, m.items[0].value);
    TEST_ASSERT_EQUAL_INT64(9, m.items[1].value);
}

static void test_matching_is_by_content_not_pointer(void)
{
    sentry_metrics_t m;
    char name[] = "heap";
    sentry_metrics_reset(&m);

    /* The fast path compares pointers because names are literals in practice, but a name
     * built at runtime must still land in the same slot rather than filling the table. */
    sentry_metrics_count(&m, "heap", 1, NULL);
    sentry_metrics_count(&m, name, 1, NULL);

    TEST_ASSERT_EQUAL_INT64(2, m.items[0].value);
    TEST_ASSERT_FALSE(m.items[1].used);
}

static void test_a_full_table_drops_the_new_name_not_a_running_total(void)
{
    sentry_metrics_t m;
    static const char *names[] = { "a", "b", "c", "d", "e", "f", "g", "h" };
    sentry_metrics_reset(&m);

    for (int i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        TEST_ASSERT_TRUE(sentry_metrics_count(&m, names[i], 10, NULL));
    }
    TEST_ASSERT_FALSE(sentry_metrics_count(&m, "one-too-many", 1, NULL));
    TEST_ASSERT_EQUAL_UINT(1, m.dropped);

    /* Evicting an accumulating total would be worse than refusing a new name: only the
     * second is visible to anyone. */
    TEST_ASSERT_EQUAL_INT64(10, m.items[0].value);
    TEST_ASSERT_TRUE(sentry_metrics_count(&m, "a", 5, NULL));
    TEST_ASSERT_EQUAL_INT64(15, m.items[0].value);
}

static void test_empty_until_something_is_recorded(void)
{
    sentry_metrics_t m;
    sentry_metrics_reset(&m);

    /* The common state between flushes, and what stops flush touching the transport. */
    TEST_ASSERT_TRUE(sentry_metrics_empty(&m));
    sentry_metrics_count(&m, "x", 1, NULL);
    TEST_ASSERT_FALSE(sentry_metrics_empty(&m));
    sentry_metrics_reset(&m);
    TEST_ASSERT_TRUE(sentry_metrics_empty(&m));
}

static void test_writes_a_trace_metric_envelope(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);

    sentry_metrics_count(&m, "ble.disconnect", 3, NULL);
    sentry_metrics_gauge(&m, "device.free_heap", 201716, "byte");

    size_t len = sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"trace_metric\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"item_count\":2"));
    TEST_ASSERT_NOT_NULL(
        strstr(buf, "\"content_type\":\"application/vnd.sentry.items.trace-metric+json\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"ble.disconnect\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"counter\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"value\":3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"gauge\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit\":\"byte\""));
    /* Required on every metric by the protocol. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" TRACE "\""));
    /* A number, not a string — a quoted value would not aggregate. */
    TEST_ASSERT_NULL(strstr(buf, "\"value\":\""));
}

static void test_no_trace_means_no_envelope(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);
    sentry_metrics_count(&m, "x", 1, NULL);

    /* The protocol requires a trace id on every metric, so sending without one would be
     * rejected server-side after the radio had already been paid for. */
    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, ""));
    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, NULL));
}

static void test_an_empty_table_writes_nothing(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);

    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_a_negative_gauge_survives_the_round_trip(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);

    /* RSSI is the obvious one, and an unsigned write would turn -67 into 4294967229. */
    sentry_metrics_gauge(&m, "wifi.rssi", -67, NULL);
    TEST_ASSERT_TRUE(sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"value\":-67"));
}

static void test_null_is_safe(void)
{
    TEST_ASSERT_FALSE(sentry_metrics_count(NULL, "x", 1, NULL));
    TEST_ASSERT_TRUE(sentry_metrics_empty(NULL));
    sentry_metrics_reset(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_counter_accumulates_in_one_slot);
    RUN_TEST(test_a_gauge_keeps_the_latest_reading);
    RUN_TEST(test_a_counter_and_a_gauge_of_the_same_name_are_different_metrics);
    RUN_TEST(test_matching_is_by_content_not_pointer);
    RUN_TEST(test_a_full_table_drops_the_new_name_not_a_running_total);
    RUN_TEST(test_empty_until_something_is_recorded);
    RUN_TEST(test_writes_a_trace_metric_envelope);
    RUN_TEST(test_no_trace_means_no_envelope);
    RUN_TEST(test_an_empty_table_writes_nothing);
    RUN_TEST(test_a_negative_gauge_survives_the_round_trip);
    RUN_TEST(test_null_is_safe);
    return UNITY_END();
}
