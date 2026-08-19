/**
 * Host tests for trace context parsing.
 *
 * This is a header parser, so every bug in it is a wrong association rather than a crash:
 * the event still sends, Sentry still accepts it, and it joins a trace that does not exist
 * or belongs to somebody else. Nothing about the result looks wrong until you go looking
 * for the other end of the link — the same silent failure as a mismatched `debug_id`.
 *
 * So most of what is below is malformed input. Getting the happy path right is easy; the
 * value is in refusing everything else rather than believing half of it.
 */

#include <string.h>
#include <unity.h>

#include "sentry_trace.h"

#define TRACE "d49d9bf66f13450b81f65bc51cf49c03"
#define PARENT "7c51afd529da4a2a"
#define REPLAY "1c4b1a2f3e5d4c6b8a9f0e1d2c3b4a59"

static const uint8_t SPAN_BYTES[8] = { 0xbb, 0x8f, 0x27, 0x81, 0x30, 0x53, 0x5c, 0x3c };
static const uint8_t TRACE_BYTES[16] = { 0x8f, 0x43, 0x1b, 0x7a, 0xa0, 0x84, 0x41, 0xbb, 0xbd, 0x5a,
    0x01, 0x00, 0xfd, 0x91, 0xf9, 0xfe };

void setUp(void) { }
void tearDown(void) { }

static void test_formats_ids_as_lowercase_hex(void)
{
    char trace_id[SENTRY_MICRO_TRACE_ID_LEN];
    char span_id[SENTRY_MICRO_SPAN_ID_LEN];

    sentry_trace_id_format(trace_id, TRACE_BYTES);
    sentry_span_id_format(span_id, SPAN_BYTES);

    TEST_ASSERT_EQUAL_STRING("8f431b7aa08441bbbd5a0100fd91f9fe", trace_id);
    TEST_ASSERT_EQUAL_STRING("bb8f278130535c3c", span_id);
}

static void test_adopts_a_well_formed_header(void)
{
    sentry_trace_context_t ctx;
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&ctx, TRACE "-" PARENT "-1", NULL, SPAN_BYTES));

    TEST_ASSERT_TRUE(ctx.active);
    TEST_ASSERT_EQUAL_STRING(TRACE, ctx.trace_id);
    /* The incoming span becomes our parent; we mint our own. */
    TEST_ASSERT_EQUAL_STRING(PARENT, ctx.parent_span_id);
    TEST_ASSERT_EQUAL_STRING("bb8f278130535c3c", ctx.span_id);
    TEST_ASSERT_TRUE(ctx.sampled);
    TEST_ASSERT_TRUE(ctx.has_sampling_decision);
}

static void test_a_deferred_sampling_decision_stays_deferred(void)
{
    sentry_trace_context_t ctx;
    /* No third field: the caller has not decided. Inventing one here would make this
     * device the origin of a decision the rest of the trace never agreed to. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&ctx, TRACE "-" PARENT, NULL, SPAN_BYTES));
    TEST_ASSERT_FALSE(ctx.has_sampling_decision);
    TEST_ASSERT_FALSE(ctx.sampled);
}

static void test_an_unsampled_trace_is_still_adopted(void)
{
    sentry_trace_context_t ctx;
    /* Errors are never sampled away, so the crash still needs the trace id to link on. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&ctx, TRACE "-" PARENT "-0", NULL, SPAN_BYTES));
    TEST_ASSERT_TRUE(ctx.active);
    TEST_ASSERT_FALSE(ctx.sampled);
    TEST_ASSERT_TRUE(ctx.has_sampling_decision);
}

static void assert_rejected(const char *header)
{
    sentry_trace_context_t ctx;
    TEST_ASSERT_FALSE_MESSAGE(sentry_trace_adopt_header(&ctx, header, NULL, SPAN_BYTES), header);
    /* Rejected means nothing adopted — not "adopted the readable part". */
    TEST_ASSERT_FALSE(ctx.active);
    TEST_ASSERT_EQUAL_STRING("", ctx.trace_id);
}

static void test_rejects_malformed_headers_wholesale(void)
{
    assert_rejected("");
    assert_rejected("nonsense");
    assert_rejected(TRACE); /* no span */
    assert_rejected(TRACE "-"); /* empty span */
    assert_rejected(TRACE "-7c51afd529da4a"); /* span one short */
    assert_rejected(TRACE "-7c51afd529da4a2a2"); /* span one long */
    assert_rejected("d49d9bf66f13450b81f65bc51cf49c0-" PARENT); /* trace one short */
    assert_rejected("D49D9BF66F13450B81F65BC51CF49C03-" PARENT); /* uppercase */
    assert_rejected("d49d9bf66f13450b81f65bc51cf49cg3-" PARENT); /* not hex */
    assert_rejected(TRACE "_" PARENT); /* wrong separator */
    assert_rejected(TRACE "-" PARENT "-2"); /* bad sampled flag */
    assert_rejected(TRACE "-" PARENT "-1-extra"); /* trailing junk */
}

static void test_rejects_an_all_zero_id(void)
{
    /* Some senders spell "no trace" as all zeroes. Adopting it would drop every such
     * device's events into one shared fictional trace. */
    assert_rejected("00000000000000000000000000000000-" PARENT "-1");
    assert_rejected(TRACE "-0000000000000000-1");
}

static void test_a_null_header_is_not_a_trace(void)
{
    sentry_trace_context_t ctx;
    TEST_ASSERT_FALSE(sentry_trace_adopt_header(&ctx, NULL, NULL, SPAN_BYTES));
    TEST_ASSERT_FALSE(ctx.active);
}

static void test_picks_the_replay_id_out_of_baggage(void)
{
    sentry_trace_context_t ctx;
    const char *baggage
        = "sentry-environment=production,sentry-replay_id=" REPLAY ",sentry-public_key=abc123";
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&ctx, TRACE "-" PARENT "-1", baggage, SPAN_BYTES));
    TEST_ASSERT_EQUAL_STRING(REPLAY, ctx.replay_id);
}

static void test_tolerates_baggage_whitespace_and_position(void)
{
    sentry_trace_context_t ctx;
    /* W3C allows spaces around the separators, and the key can be last. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT, "sentry-release=1.0 , sentry-replay_id=" REPLAY, SPAN_BYTES));
    TEST_ASSERT_EQUAL_STRING(REPLAY, ctx.replay_id);
}

static void test_a_bad_replay_id_does_not_cost_the_trace(void)
{
    sentry_trace_context_t ctx;
    /* The replay link is a bonus. Losing the trace because the optional extra was garbled
     * would trade something valuable for something merely nice. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT "-1", "sentry-replay_id=not-a-replay", SPAN_BYTES));
    TEST_ASSERT_TRUE(ctx.active);
    TEST_ASSERT_EQUAL_STRING("", ctx.replay_id);
}

static void test_no_replay_id_is_ordinary(void)
{
    sentry_trace_context_t ctx;
    /* Only an app with replay running sends one. Its absence is not an error. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT "-1", "sentry-environment=production", SPAN_BYTES));
    TEST_ASSERT_TRUE(ctx.active);
    TEST_ASSERT_EQUAL_STRING("", ctx.replay_id);
}

static void test_picks_the_org_id_out_of_baggage(void)
{
    sentry_trace_context_t ctx;
    const char *baggage = "sentry-environment=production,sentry-org_id=1234";
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&ctx, TRACE "-" PARENT "-1", baggage, SPAN_BYTES));
    TEST_ASSERT_EQUAL_STRING("1234", ctx.org_id);
}

static void test_a_non_digit_org_id_does_not_cost_the_trace(void)
{
    sentry_trace_context_t ctx;
    /* Same rule as a garbled replay_id: the org id is a bonus for a later comparison, not
     * a reason to lose the trace it arrived on. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT "-1", "sentry-org_id=not-a-number", SPAN_BYTES));
    TEST_ASSERT_TRUE(ctx.active);
    TEST_ASSERT_EQUAL_STRING("", ctx.org_id);
}

static void test_no_org_id_is_ordinary(void)
{
    sentry_trace_context_t ctx;
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT "-1", "sentry-replay_id=" REPLAY, SPAN_BYTES));
    TEST_ASSERT_EQUAL_STRING("", ctx.org_id);
}

static void test_does_not_confuse_a_key_that_merely_ends_the_same(void)
{
    sentry_trace_context_t ctx;
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT, "not-sentry-replay_id=" REPLAY, SPAN_BYTES));
    TEST_ASSERT_EQUAL_STRING("", ctx.replay_id);
}

static void test_begins_a_device_originated_trace(void)
{
    sentry_trace_context_t ctx;
    sentry_trace_begin(&ctx, TRACE_BYTES, SPAN_BYTES, true);

    TEST_ASSERT_TRUE(ctx.active);
    TEST_ASSERT_EQUAL_STRING("8f431b7aa08441bbbd5a0100fd91f9fe", ctx.trace_id);
    /* Nobody called us, so there is no parent — not a zeroed one. */
    TEST_ASSERT_EQUAL_STRING("", ctx.parent_span_id);
    TEST_ASSERT_TRUE(ctx.has_sampling_decision);
}

static void test_clearing_leaves_nothing_behind(void)
{
    sentry_trace_context_t ctx;
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(
        &ctx, TRACE "-" PARENT "-1", "sentry-replay_id=" REPLAY ",sentry-org_id=1234", SPAN_BYTES));

    sentry_trace_clear(&ctx);

    /* The whole point of releasing: a later panic must not be attributed to this request. */
    TEST_ASSERT_FALSE(ctx.active);
    TEST_ASSERT_EQUAL_STRING("", ctx.trace_id);
    TEST_ASSERT_EQUAL_STRING("", ctx.replay_id);
    TEST_ASSERT_EQUAL_STRING("", ctx.org_id);
}

static void test_writes_a_header_for_an_outbound_call(void)
{
    sentry_trace_context_t ctx;
    char buf[64];

    sentry_trace_begin(&ctx, TRACE_BYTES, SPAN_BYTES, true);
    TEST_ASSERT_EQUAL_UINT(51, sentry_trace_header_write(buf, sizeof(buf), &ctx));
    TEST_ASSERT_EQUAL_STRING("8f431b7aa08441bbbd5a0100fd91f9fe-bb8f278130535c3c-1", buf);
}

static void test_round_trips_through_its_own_header(void)
{
    sentry_trace_context_t sent, received;
    char buf[64];

    sentry_trace_begin(&sent, TRACE_BYTES, SPAN_BYTES, true);
    sentry_trace_header_write(buf, sizeof(buf), &sent);

    /* What this device emits, another participant must be able to adopt. */
    TEST_ASSERT_TRUE(sentry_trace_adopt_header(&received, buf, NULL, SPAN_BYTES));
    TEST_ASSERT_EQUAL_STRING(sent.trace_id, received.trace_id);
    TEST_ASSERT_EQUAL_STRING(sent.span_id, received.parent_span_id);
}

static void test_writes_nothing_when_no_trace_is_active(void)
{
    sentry_trace_context_t ctx;
    char buf[64] = "untouched";

    sentry_trace_clear(&ctx);
    /* 0 means "send no header", which lets the far end start its own trace rather than
     * joining one that does not exist. */
    TEST_ASSERT_EQUAL_UINT(0, sentry_trace_header_write(buf, sizeof(buf), &ctx));
}

static void test_refuses_to_write_into_too_small_a_buffer(void)
{
    sentry_trace_context_t ctx;
    char buf[16];

    sentry_trace_begin(&ctx, TRACE_BYTES, SPAN_BYTES, true);
    TEST_ASSERT_EQUAL_UINT(0, sentry_trace_header_write(buf, sizeof(buf), &ctx));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_formats_ids_as_lowercase_hex);
    RUN_TEST(test_adopts_a_well_formed_header);
    RUN_TEST(test_a_deferred_sampling_decision_stays_deferred);
    RUN_TEST(test_an_unsampled_trace_is_still_adopted);
    RUN_TEST(test_rejects_malformed_headers_wholesale);
    RUN_TEST(test_rejects_an_all_zero_id);
    RUN_TEST(test_a_null_header_is_not_a_trace);
    RUN_TEST(test_picks_the_replay_id_out_of_baggage);
    RUN_TEST(test_tolerates_baggage_whitespace_and_position);
    RUN_TEST(test_a_bad_replay_id_does_not_cost_the_trace);
    RUN_TEST(test_no_replay_id_is_ordinary);
    RUN_TEST(test_picks_the_org_id_out_of_baggage);
    RUN_TEST(test_a_non_digit_org_id_does_not_cost_the_trace);
    RUN_TEST(test_no_org_id_is_ordinary);
    RUN_TEST(test_does_not_confuse_a_key_that_merely_ends_the_same);
    RUN_TEST(test_begins_a_device_originated_trace);
    RUN_TEST(test_clearing_leaves_nothing_behind);
    RUN_TEST(test_writes_a_header_for_an_outbound_call);
    RUN_TEST(test_round_trips_through_its_own_header);
    RUN_TEST(test_writes_nothing_when_no_trace_is_active);
    RUN_TEST(test_refuses_to_write_into_too_small_a_buffer);
    return UNITY_END();
}
