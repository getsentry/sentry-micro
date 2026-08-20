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
/* 2026-08-19T00:00:00Z in microseconds. */
#define NOW_US 1755561600000000ULL

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

/*
 * The budget the flush path actually writes into, which nothing here checked before.
 *
 * A metric name is stored as the caller's pointer and never copied (see sentry_metric_t), so
 * its length is unbounded, and it is JSON-escaped on the way out like anything else. That
 * makes a table which does not fit SENTRY_MICRO_ENVELOPE_BUFFER_BYTES reachable rather than
 * theoretical — and an over-budget table used to stop metrics for the rest of the boot,
 * since flush_metrics() returned without clearing it and a full table also refuses new names.
 */
static void test_a_full_table_of_ordinary_names_fits_the_envelope_budget(void)
{
    sentry_metrics_t m;
    sentry_metrics_reset(&m);
    static const char *names[SENTRY_MICRO_MAX_METRICS] = {
        "heap.free",
        "heap.min_free",
        "wifi.rssi",
        "battery.mv",
        "render.frame_us",
        "ota.duration_ms",
        "ble.throughput",
        "loop.iterations",
    };
    for (int i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        sentry_metrics_gauge(&m, names[i], 1234567, "byte");
    }

    size_t needed = sentry_metrics_envelope_write(NULL, 0, &m, TRACE, NOW_US);
    TEST_ASSERT_TRUE(needed > 0);
    TEST_ASSERT_TRUE(needed <= SENTRY_MICRO_ENVELOPE_BUFFER_BYTES);
}

static void test_an_over_budget_table_is_reported_honestly_and_writes_nothing(void)
{
    sentry_metrics_t m;
    static char names[SENTRY_MICRO_MAX_METRICS][129];
    sentry_metrics_reset(&m);

    /* Long but perfectly legal names. Eight of these encode past the budget, which is what
     * flush_metrics() has to detect in order to drop the batch instead of retrying it. */
    for (int i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        memset(names[i], 'n', sizeof(names[i]) - 1);
        names[i][sizeof(names[i]) - 1] = '\0';
        names[i][0] = (char)('a' + i);
        sentry_metrics_gauge(&m, names[i], 42, "byte");
    }

    size_t needed = sentry_metrics_envelope_write(NULL, 0, &m, TRACE, NOW_US);
    TEST_ASSERT_TRUE(needed > SENTRY_MICRO_ENVELOPE_BUFFER_BYTES);

    /* Over budget must mean nothing written, not a truncated envelope that still looks
     * plausible — that report is the only thing the flush path decides on. */
    char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    TEST_ASSERT_EQUAL_size_t(
        needed, sentry_metrics_envelope_write(envelope, sizeof(envelope), &m, TRACE, NOW_US));
    TEST_ASSERT_EQUAL_STRING("", envelope);
}

static void test_escaping_in_a_name_counts_toward_the_budget(void)
{
    sentry_metrics_t plain;
    sentry_metrics_t escaped;
    static char plain_names[SENTRY_MICRO_MAX_METRICS][65];
    static char quoted_names[SENTRY_MICRO_MAX_METRICS][65];
    sentry_metrics_reset(&plain);
    sentry_metrics_reset(&escaped);

    for (int i = 0; i < SENTRY_MICRO_MAX_METRICS; i++) {
        memset(plain_names[i], 'n', sizeof(plain_names[i]) - 1);
        memset(quoted_names[i], '"', sizeof(quoted_names[i]) - 1);
        plain_names[i][sizeof(plain_names[i]) - 1] = '\0';
        quoted_names[i][sizeof(quoted_names[i]) - 1] = '\0';
        plain_names[i][0] = (char)('a' + i);
        quoted_names[i][0] = (char)('a' + i);
        sentry_metrics_gauge(&plain, plain_names[i], 42, "byte");
        sentry_metrics_gauge(&escaped, quoted_names[i], 42, "byte");
    }

    /* Same raw name length; the measurement has to come from the encoder, the same property
     * test_escaping_counts_toward_the_envelope_budget() guards for logs. */
    size_t plain_needed = sentry_metrics_envelope_write(NULL, 0, &plain, TRACE, NOW_US);
    size_t escaped_needed = sentry_metrics_envelope_write(NULL, 0, &escaped, TRACE, NOW_US);
    TEST_ASSERT_TRUE(plain_needed <= SENTRY_MICRO_ENVELOPE_BUFFER_BYTES);
    TEST_ASSERT_TRUE(escaped_needed > plain_needed);
    TEST_ASSERT_TRUE(escaped_needed > SENTRY_MICRO_ENVELOPE_BUFFER_BYTES);
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

    size_t len = sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE, NOW_US);
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
    /* Required on every metric. The spec's own example payloads omit it, so this was built
     * without one at first: ingest accepted the envelope at the edge and dropped every
     * metric inside it, with nothing anywhere reporting the loss. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561600.000000"));
}

static void test_no_clock_means_no_envelope(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);
    sentry_metrics_count(&m, "x", 1, NULL);

    /* Refused rather than stamped at the epoch. The caller holds the table and tries again
     * once the device has been told the date — a counter covering a longer interval is
     * still true, unlike a duration, which is why metrics wait where transactions drop. */
    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE, 0));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_no_trace_means_no_envelope(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);
    sentry_metrics_count(&m, "x", 1, NULL);

    /* The protocol requires a trace id on every metric, so sending without one would be
     * rejected server-side after the radio had already been paid for. */
    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, "", NOW_US));
    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, NULL, NOW_US));
}

static void test_an_empty_table_writes_nothing(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);

    TEST_ASSERT_EQUAL_UINT(0, sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE, NOW_US));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_a_negative_gauge_survives_the_round_trip(void)
{
    sentry_metrics_t m;
    char buf[2048];
    sentry_metrics_reset(&m);

    /* RSSI is the obvious one, and an unsigned write would turn -67 into 4294967229. */
    sentry_metrics_gauge(&m, "wifi.rssi", -67, NULL);
    TEST_ASSERT_TRUE(sentry_metrics_envelope_write(buf, sizeof(buf), &m, TRACE, NOW_US) > 0);
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
    RUN_TEST(test_a_full_table_of_ordinary_names_fits_the_envelope_budget);
    RUN_TEST(test_an_over_budget_table_is_reported_honestly_and_writes_nothing);
    RUN_TEST(test_escaping_in_a_name_counts_toward_the_budget);
    RUN_TEST(test_writes_a_trace_metric_envelope);
    RUN_TEST(test_no_trace_means_no_envelope);
    RUN_TEST(test_no_clock_means_no_envelope);
    RUN_TEST(test_an_empty_table_writes_nothing);
    RUN_TEST(test_a_negative_gauge_survives_the_round_trip);
    RUN_TEST(test_null_is_safe);
    return UNITY_END();
}
