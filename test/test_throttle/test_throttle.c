/**
 * Host tests for the capture throttle.
 *
 * This is the one piece of `sentry_capture_message()` that can be wrong in a way nobody
 * notices. A throttle that is too tight silently drops the event that mattered; one that
 * is too loose silently bills a quota. Neither shows up on a bench, because both need a
 * device left running for hours to become visible — so the behaviour is pinned here, where
 * a day of traffic costs a microsecond.
 *
 * The clock is a parameter rather than a call, which is the whole reason this logic lives
 * in `core/`: every case below drives time forward by hand.
 */

#include <stdio.h>
#include <unity.h>

#include "sentry_throttle.h"

/* Generous limits, so a test that is not about a particular rule is not tripped by it. */
static void init_default(sentry_throttle_t *throttle) { sentry_throttle_init(throttle, 100, 1000); }

void setUp(void) { }
void tearDown(void) { }

static void test_lets_an_ordinary_message_through(void)
{
    sentry_throttle_t throttle;
    init_default(&throttle);

    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "hello", 0));
    TEST_ASSERT_EQUAL_UINT32(0, sentry_throttle_suppressed(&throttle));
}

static void test_suppresses_an_immediate_repeat(void)
{
    sentry_throttle_t throttle;
    init_default(&throttle);

    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "sensor read failed", 0));
    /* The failing-sensor-in-a-loop case: same text, microseconds later. */
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_FALSE(
            sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "sensor read failed", 10 + i));
    }
    TEST_ASSERT_EQUAL_UINT32(50, sentry_throttle_suppressed(&throttle));
}

static void test_lets_the_same_message_through_again_after_the_window(void)
{
    sentry_throttle_t throttle;
    init_default(&throttle);

    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "again", 0));
    TEST_ASSERT_FALSE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "again", 999));
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "again", 1000));
}

static void test_a_tight_loop_still_reports_once_per_window(void)
{
    sentry_throttle_t throttle;
    init_default(&throttle);

    /* The reason the repeat timestamp advances only on an allowed message. If it slid
     * forward on every suppressed call, a caller faster than the window would go silent
     * after the first event and never report again — the failure would look like it had
     * stopped happening. */
    uint32_t allowed = 0;
    for (uint64_t now = 0; now < 5000; now += 10) {
        if (sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "spinning", now)) {
            allowed++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(5, allowed);
}

static void test_a_repeat_does_not_spend_the_volume_budget(void)
{
    sentry_throttle_t throttle;
    /* Two per minute, so the budget is easy to exhaust if repeats wrongly consume it. */
    sentry_throttle_init(&throttle, 2, 60000);

    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_WARNING, "flooding", 0));
    for (int i = 0; i < 100; i++) {
        (void)sentry_throttle_allow(&throttle, SENTRY_LEVEL_WARNING, "flooding", 1);
    }
    /* One budget slot went to "flooding"; the second is still there for something else. */
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_WARNING, "something else", 2));
    TEST_ASSERT_FALSE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_WARNING, "a third thing", 3));
}

static void test_distinct_messages_hit_the_per_minute_ceiling(void)
{
    sentry_throttle_t throttle;
    sentry_throttle_init(&throttle, 3, 0);

    /* Distinct every time, so only the volume rule can stop this — the backstop for a
     * message with a counter in it, which repeat suppression cannot see. */
    char message[32];
    uint32_t allowed = 0;
    for (int i = 0; i < 20; i++) {
        snprintf(message, sizeof(message), "attempt %d failed", i);
        if (sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, message, 100)) {
            allowed++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3, allowed);
    TEST_ASSERT_EQUAL_UINT32(17, sentry_throttle_suppressed(&throttle));
}

static void test_the_volume_budget_refills_next_window(void)
{
    sentry_throttle_t throttle;
    sentry_throttle_init(&throttle, 1, 0);

    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "one", 0));
    TEST_ASSERT_FALSE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "two", 59999));
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "three", 60000));
}

static void test_the_same_text_at_a_different_level_is_a_different_message(void)
{
    sentry_throttle_t throttle;
    init_default(&throttle);

    /* "Retrying" as info and "Retrying" as error are not the same event, and collapsing
     * them would hide an escalation. */
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "retrying", 0));
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "retrying", 1));
    TEST_ASSERT_FALSE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_ERROR, "retrying", 2));
}

static void test_zero_limits_disable_each_rule(void)
{
    sentry_throttle_t throttle;
    sentry_throttle_init(&throttle, 0, 0);

    /* Both off: an unlimited firehose, which is what a user who set 0 asked for. */
    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "same text", 0));
    }
    TEST_ASSERT_EQUAL_UINT32(0, sentry_throttle_suppressed(&throttle));
}

static void test_a_null_message_is_allowed_not_dropped(void)
{
    sentry_throttle_t throttle;
    init_default(&throttle);

    /* The level, timestamp and device context are still worth an event; refusing one
     * because the text was missing would throw all of that away. */
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, NULL, 0));
    /* And it deduplicates like any other message. */
    TEST_ASSERT_FALSE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, NULL, 1));
}

static void test_a_null_throttle_allows_everything(void)
{
    /* Never the reason an event goes missing. */
    TEST_ASSERT_TRUE(sentry_throttle_allow(NULL, SENTRY_LEVEL_FATAL, "still fine", 0));
    TEST_ASSERT_EQUAL_UINT32(0, sentry_throttle_suppressed(NULL));
}

static void test_survives_a_long_uptime(void)
{
    sentry_throttle_t throttle;
    sentry_throttle_init(&throttle, 1, 1000);

    /* 50 days of uptime, past where a 32-bit millisecond counter would have wrapped.
     * uptime_ms is 64-bit for exactly this reason, and the arithmetic here has to agree. */
    const uint64_t late = 50ull * 24 * 60 * 60 * 1000;
    TEST_ASSERT_TRUE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "still here", late));
    TEST_ASSERT_FALSE(sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "still here", late + 1));
    TEST_ASSERT_TRUE(
        sentry_throttle_allow(&throttle, SENTRY_LEVEL_INFO, "still here", late + 60000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lets_an_ordinary_message_through);
    RUN_TEST(test_suppresses_an_immediate_repeat);
    RUN_TEST(test_lets_the_same_message_through_again_after_the_window);
    RUN_TEST(test_a_tight_loop_still_reports_once_per_window);
    RUN_TEST(test_a_repeat_does_not_spend_the_volume_budget);
    RUN_TEST(test_distinct_messages_hit_the_per_minute_ceiling);
    RUN_TEST(test_the_volume_budget_refills_next_window);
    RUN_TEST(test_the_same_text_at_a_different_level_is_a_different_message);
    RUN_TEST(test_zero_limits_disable_each_rule);
    RUN_TEST(test_a_null_message_is_allowed_not_dropped);
    RUN_TEST(test_a_null_throttle_allows_everything);
    RUN_TEST(test_survives_a_long_uptime);
    return UNITY_END();
}
