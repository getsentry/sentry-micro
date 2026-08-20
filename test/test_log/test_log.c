/**
 * Host tests for the log ring.
 *
 * Two properties matter here that do not for metrics: entries are not aggregated — each one
 * is distinct and keeps its own trace_id, recorded rather than resolved once at flush — and
 * the ring evicts the oldest entry when full rather than refusing the newest, because there
 * is no running total here worth protecting.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "sentry_log.h"

#define TRACE "d49d9bf66f13450b81f65bc51cf49c03"
#define FALLBACK_TRACE "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define DEVICE_ID "d4e9f486ed14"
/* 2026-08-19T00:00:00Z in microseconds. */
#define NOW_UNIX_US 1755561600000000ULL

void setUp(void) { }
void tearDown(void) { }

static void test_a_line_is_recorded(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);

    TEST_ASSERT_TRUE(sentry_log_ring_empty(&r));
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "boot complete");
    TEST_ASSERT_FALSE(sentry_log_ring_empty(&r));
    TEST_ASSERT_EQUAL_UINT(1, r.count);
    TEST_ASSERT_EQUAL_STRING("boot complete", r.entries[0].body);
    TEST_ASSERT_EQUAL_STRING(TRACE, r.entries[0].trace_id);
}

static void test_lines_are_not_aggregated(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);

    /* Unlike a metric, two identical bodies are two entries, not one accumulated total. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "tick");
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 2000, "tick");
    TEST_ASSERT_EQUAL_UINT(2, r.count);
}

static void test_a_full_ring_evicts_the_oldest_not_the_newest(void)
{
    sentry_log_ring_t r;
    char body[16];
    sentry_log_ring_reset(&r);

    for (int i = 0; i < SENTRY_MICRO_MAX_LOGS; i++) {
        snprintf(body, sizeof(body), "line-%d", i);
        TEST_ASSERT_FALSE(
            sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, (uint64_t)i * 1000, body));
    }
    TEST_ASSERT_EQUAL_UINT(SENTRY_MICRO_MAX_LOGS, r.count);
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);

    /* One more evicts line-0, not line-7: the newest line is worth more than the one it
     * replaces, unlike a metrics table protecting a running total. The return value is the
     * production signal sentry_log()/logs_dropped_count() actually depend on — not just the
     * ring's own internal counter, which happens to move for the same reason but is a
     * separate field. */
    TEST_ASSERT_TRUE(sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 99000, "one-too-many"));
    TEST_ASSERT_EQUAL_UINT(SENTRY_MICRO_MAX_LOGS, r.count);
    TEST_ASSERT_EQUAL_UINT(1, r.dropped);

    char buf[2048];
    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 100000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "\"line-0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"one-too-many\""));
}

static void test_entries_serialise_oldest_first(void)
{
    sentry_log_ring_t r;
    char body[16];
    sentry_log_ring_reset(&r);

    for (int i = 0; i < SENTRY_MICRO_MAX_LOGS; i++) {
        snprintf(body, sizeof(body), "line-%d", i);
        sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, (uint64_t)i * 1000, body);
    }
    /* One more evicts line-0, so the ring now holds line-1..line-8 in that order. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 8000, "line-8");

    char buf[2048];
    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 9000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));

    const char *p2 = strstr(buf, "\"line-2\"");
    const char *p8 = strstr(buf, "\"line-8\"");
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_NULL(p8);
    TEST_ASSERT_TRUE(p2 < p8);
}

static void test_an_overlong_body_is_truncated_not_dropped(void)
{
    sentry_log_ring_t r;
    char long_body[200];
    sentry_log_ring_reset(&r);

    memset(long_body, 'x', sizeof(long_body) - 1);
    long_body[sizeof(long_body) - 1] = '\0';
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, long_body);

    TEST_ASSERT_EQUAL_UINT(1, r.count);
    /* Truncated to fit, not rejected: a shortened line the operator can still read beats
     * losing it entirely. */
    TEST_ASSERT_EQUAL_UINT(SENTRY_MICRO_LOG_BODY_LEN - 1, strlen(r.entries[0].body));
}

static void test_recorded_trace_id_wins_over_the_fallback(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "during a real operation");

    char buf[2048];
    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" TRACE "\""));
    TEST_ASSERT_NULL(strstr(buf, "\"trace_id\":\"" FALLBACK_TRACE "\""));
}

static void test_an_idle_recorded_line_falls_back_to_the_batch_trace(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    /* Empty trace_id: nothing was active when this line was recorded. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, "", 1000, "while idle");

    char buf[2048];
    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" FALLBACK_TRACE "\""));
}

static void test_a_batch_may_mix_entries_from_different_traces(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "during the operation");
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, "", 2000, "after it ended");

    char buf[2048];
    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 3000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" TRACE "\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" FALLBACK_TRACE "\""));
}

static void test_writes_a_log_envelope(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);

    sentry_log_ring_push(&r, SENTRY_LEVEL_WARNING, TRACE, 1000, "reconnect attempt 3");

    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 501000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"log\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"item_count\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"content_type\":\"application/vnd.sentry.items.log+json\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"body\":\"reconnect attempt 3\""));
    /* SENTRY_LEVEL_WARNING is the one level that does not map onto its own name. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"level\":\"warn\""));
    TEST_ASSERT_NULL(strstr(buf, "\"level\":\"warning\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"severity_number\":13"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"trace_id\":\"" TRACE "\""));
    TEST_ASSERT_NOT_NULL(strstr(
        buf, "\"attributes\":{\"device_id\":{\"value\":\"" DEVICE_ID "\",\"type\":\"string\"}}"));

    /* Entry recorded 500ms of uptime before the flush anchor: its timestamp is the flush's
     * wall-clock anchor minus that same 500ms, not the flush time itself. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561599.500000"));
}

static void test_entry_unix_us_clamps_instead_of_underflowing(void)
{
    sentry_log_ring_t r;
    char buf[2048];

    /* An entry that looks newer than the flush anchor should not happen, but if it did,
     * elapsed clamps to 0 rather than underflowing — the entry reads as "now", not as a
     * huge wrapped uint64. */
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 5000, "x");
    TEST_ASSERT_TRUE(sentry_log_envelope_write(
                         buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 1000, NOW_UNIX_US)
        > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561600.000000"));

    /* An elapsed uptime longer than the wall clock itself (unreachable in practice — no
     * device has decades of continuous uptime — but the arithmetic must not wrap) clamps to
     * the flush's own anchor instead of underflowing past it. */
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 0, "x");
    TEST_ASSERT_TRUE(sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID,
                         NOW_UNIX_US + 1000000, NOW_UNIX_US)
        > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561600.000000"));
}

static void test_envelope_write_reports_the_size_it_needs(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "boot complete");

    /* Dry run, then a real one that must agree. */
    size_t needed
        = sentry_log_envelope_write(NULL, 0, &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(needed > 0);

    char buf[2048];
    TEST_ASSERT_EQUAL_size_t(needed,
        sentry_log_envelope_write(
            buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US));

    /* Too small: report the requirement, and emit nothing that could be mistaken for a
     * complete envelope. */
    char small[16];
    TEST_ASSERT_EQUAL_size_t(needed,
        sentry_log_envelope_write(
            small, sizeof(small), &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US));
    TEST_ASSERT_EQUAL_STRING("", small);
}

static void test_every_level_maps_to_its_log_name_and_severity(void)
{
    static const struct {
        sentry_level_t level;
        const char *name;
        int severity;
    } cases[] = {
        { SENTRY_LEVEL_DEBUG, "debug", 5 },
        { SENTRY_LEVEL_INFO, "info", 9 },
        { SENTRY_LEVEL_WARNING, "warn", 13 },
        { SENTRY_LEVEL_ERROR, "error", 17 },
        { SENTRY_LEVEL_FATAL, "fatal", 21 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sentry_log_ring_t r;
        char buf[2048];
        char expect_level[32];
        char expect_severity[32];
        sentry_log_ring_reset(&r);
        sentry_log_ring_push(&r, cases[i].level, TRACE, 1000, "x");

        size_t len = sentry_log_envelope_write(
            buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 1000, NOW_UNIX_US);
        TEST_ASSERT_TRUE(len > 0);

        snprintf(expect_level, sizeof(expect_level), "\"level\":\"%s\"", cases[i].name);
        snprintf(
            expect_severity, sizeof(expect_severity), "\"severity_number\":%d", cases[i].severity);
        TEST_ASSERT_NOT_NULL(strstr(buf, expect_level));
        TEST_ASSERT_NOT_NULL(strstr(buf, expect_severity));
    }
}

static void test_an_empty_ring_writes_nothing(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);

    TEST_ASSERT_EQUAL_UINT(0,
        sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 0, NOW_UNIX_US));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_no_clock_means_no_envelope(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x");

    /* Held, not dropped: the caller keeps the ring and tries again once the device has been
     * told the date, the same reasoning as metrics. */
    TEST_ASSERT_EQUAL_UINT(
        0, sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 2000, 0));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_no_fallback_trace_means_no_envelope(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x");

    TEST_ASSERT_EQUAL_UINT(
        0, sentry_log_envelope_write(buf, sizeof(buf), &r, "", DEVICE_ID, 2000, NOW_UNIX_US));
    TEST_ASSERT_EQUAL_UINT(
        0, sentry_log_envelope_write(buf, sizeof(buf), &r, NULL, DEVICE_ID, 2000, NOW_UNIX_US));
}

static void test_a_missing_device_id_omits_attributes_not_the_envelope(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x");

    size_t len
        = sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, NULL, 2000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NULL(strstr(buf, "\"attributes\""));
}

static void test_null_is_safe(void)
{
    TEST_ASSERT_TRUE(sentry_log_ring_empty(NULL));
    sentry_log_ring_reset(NULL);
    TEST_ASSERT_FALSE(sentry_log_ring_push(NULL, SENTRY_LEVEL_INFO, TRACE, 1000, "x"));

    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    TEST_ASSERT_FALSE(sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, NULL));
    TEST_ASSERT_TRUE(sentry_log_ring_empty(&r));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_line_is_recorded);
    RUN_TEST(test_lines_are_not_aggregated);
    RUN_TEST(test_a_full_ring_evicts_the_oldest_not_the_newest);
    RUN_TEST(test_entries_serialise_oldest_first);
    RUN_TEST(test_an_overlong_body_is_truncated_not_dropped);
    RUN_TEST(test_recorded_trace_id_wins_over_the_fallback);
    RUN_TEST(test_an_idle_recorded_line_falls_back_to_the_batch_trace);
    RUN_TEST(test_a_batch_may_mix_entries_from_different_traces);
    RUN_TEST(test_writes_a_log_envelope);
    RUN_TEST(test_entry_unix_us_clamps_instead_of_underflowing);
    RUN_TEST(test_envelope_write_reports_the_size_it_needs);
    RUN_TEST(test_every_level_maps_to_its_log_name_and_severity);
    RUN_TEST(test_an_empty_ring_writes_nothing);
    RUN_TEST(test_no_clock_means_no_envelope);
    RUN_TEST(test_no_fallback_trace_means_no_envelope);
    RUN_TEST(test_a_missing_device_id_omits_attributes_not_the_envelope);
    RUN_TEST(test_null_is_safe);
    return UNITY_END();
}
