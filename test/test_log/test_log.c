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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "boot complete", false);
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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "tick", false);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 2000, "tick", false);
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
            sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, (uint64_t)i * 1000, body, false));
    }
    TEST_ASSERT_EQUAL_UINT(SENTRY_MICRO_MAX_LOGS, r.count);
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);

    /* One more evicts line-0, not the newest survivor: the newest line is worth more than
     * the one it replaces, unlike a metrics table protecting a running total. The return
     * value is the production signal sentry_log()/logs_dropped_count() actually depend on
     * — not just the ring's own internal counter, which happens to move for the same
     * reason but is a separate field. */
    TEST_ASSERT_TRUE(
        sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 99000, "one-too-many", false));
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
    char one_more[16];
    sentry_log_ring_reset(&r);

    for (int i = 0; i < SENTRY_MICRO_MAX_LOGS; i++) {
        snprintf(body, sizeof(body), "line-%d", i);
        sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, (uint64_t)i * 1000, body, false);
    }
    /* One more evicts line-0, so the ring now holds line-1..line-N (N = SENTRY_MICRO_MAX_LOGS)
     * in that order. */
    snprintf(one_more, sizeof(one_more), "line-%d", SENTRY_MICRO_MAX_LOGS);
    sentry_log_ring_push(
        &r, SENTRY_LEVEL_INFO, TRACE, (uint64_t)SENTRY_MICRO_MAX_LOGS * 1000, one_more, false);

    char buf[2048];
    size_t len = sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID,
        (uint64_t)(SENTRY_MICRO_MAX_LOGS + 1) * 1000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));

    /* line-1 is the oldest survivor; the one just pushed is the newest — oldest-first means
     * line-1 has to appear before it, regardless of how many entries the ring holds. */
    char expect_oldest[16];
    snprintf(expect_oldest, sizeof(expect_oldest), "\"line-1\"");
    const char *p_oldest = strstr(buf, expect_oldest);
    char expect_newest[24];
    snprintf(expect_newest, sizeof(expect_newest), "\"%s\"", one_more);
    const char *p_newest = strstr(buf, expect_newest);
    TEST_ASSERT_NOT_NULL(p_oldest);
    TEST_ASSERT_NOT_NULL(p_newest);
    TEST_ASSERT_TRUE(p_oldest < p_newest);
}

static void test_a_full_ring_of_worst_case_entries_fits_the_envelope_budget(void)
{
    sentry_log_ring_t r;
    char max_body[SENTRY_MICRO_LOG_BODY_LEN];
    sentry_log_ring_reset(&r);

    /* A full ring of maximum-length bodies, every one of them truncated (the expensive case
     * for the conditional `t7d` attribute), with a device_id to attach.
     * sentry_log_envelope_write() writes into exactly SENTRY_MICRO_ENVELOPE_BUFFER_BYTES in
     * flush_logs() — this is the number that has to stay under that budget, not any of the
     * short-body cases the other tests use, which would not have caught
     * SENTRY_MICRO_MAX_LOGS/attribute-size regressions here.
     *
     * `x` is worst case for *raw* length only, and that is the whole limit of what this
     * test proves — see the sibling below for what escaping does to the same ring. */
    memset(max_body, 'x', sizeof(max_body) - 1);
    max_body[sizeof(max_body) - 1] = '\0';
    for (int i = 0; i < SENTRY_MICRO_MAX_LOGS; i++) {
        sentry_log_ring_push(&r, SENTRY_LEVEL_WARNING, TRACE, (uint64_t)i * 1000, max_body, true);
    }

    size_t needed
        = sentry_log_envelope_write(NULL, 0, &r, FALLBACK_TRACE, DEVICE_ID, 100000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(needed > 0);
    TEST_ASSERT_TRUE(needed <= SENTRY_MICRO_ENVELOPE_BUFFER_BYTES);
}

/*
 * The sibling of the test above, and the one that catches the regression class it cannot:
 * a body is stored raw but serialised *escaped*. sentry_json_kv_string() turns each `"` and
 * `\\` into two bytes and each control character into six, so the ring's fixed byte count
 * says nothing about what the envelope actually costs. Ordinary console text hits this — a
 * quoted string, a Windows path, a JSON payload being logged — it is not a contrived input.
 *
 * A full ring of those genuinely does not fit the budget and cannot be made to at this
 * MAX_LOGS. What has to hold is that the size probe says so *honestly*, because that report
 * is the only thing flush_logs() has to go on when it decides to drop the batch rather than
 * retry it forever.
 */
static void test_escaping_counts_toward_the_envelope_budget(void)
{
    sentry_log_ring_t plain_ring;
    sentry_log_ring_t escaped_ring;
    char plain_body[SENTRY_MICRO_LOG_BODY_LEN];
    char escaped_body[SENTRY_MICRO_LOG_BODY_LEN];

    sentry_log_ring_reset(&plain_ring);
    sentry_log_ring_reset(&escaped_ring);
    memset(plain_body, 'x', sizeof(plain_body) - 1);
    plain_body[sizeof(plain_body) - 1] = '\0';
    memset(escaped_body, '"', sizeof(escaped_body) - 1);
    escaped_body[sizeof(escaped_body) - 1] = '\0';
    for (int i = 0; i < SENTRY_MICRO_MAX_LOGS; i++) {
        sentry_log_ring_push(
            &plain_ring, SENTRY_LEVEL_WARNING, TRACE, (uint64_t)i * 1000, plain_body, true);
        sentry_log_ring_push(
            &escaped_ring, SENTRY_LEVEL_WARNING, TRACE, (uint64_t)i * 1000, escaped_body, true);
    }

    size_t plain_needed = sentry_log_envelope_write(
        NULL, 0, &plain_ring, FALLBACK_TRACE, DEVICE_ID, 100000, NOW_UNIX_US);
    size_t escaped_needed = sentry_log_envelope_write(
        NULL, 0, &escaped_ring, FALLBACK_TRACE, DEVICE_ID, 100000, NOW_UNIX_US);

    /* Two rings of identical raw size. The measurement has to come from the encoder, not
     * from strlen() — if these ever come out equal, escaping has stopped being counted. */
    TEST_ASSERT_TRUE(escaped_needed > plain_needed);

    /* And this is the batch that used to wedge the ring: over budget, so flush_logs() can
     * never send it, so it must drop it instead of holding it for a retry that will measure
     * exactly the same next time. */
    TEST_ASSERT_TRUE(escaped_needed > SENTRY_MICRO_ENVELOPE_BUFFER_BYTES);

    /* Over budget must mean nothing written, not a truncated envelope that still looks
     * plausible — same contract as test_envelope_write_reports_the_size_it_needs(), checked
     * here at exactly the buffer size flush_logs() really uses. */
    char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
    TEST_ASSERT_EQUAL_size_t(escaped_needed,
        sentry_log_envelope_write(envelope, sizeof(envelope), &escaped_ring, FALLBACK_TRACE,
            DEVICE_ID, 100000, NOW_UNIX_US));
    TEST_ASSERT_EQUAL_STRING("", envelope);
}

/* 2026-08-19T00:00:00Z; the previous boot's clock anchor in the recovery tests below. */
#define PREV_ANCHOR_UNIX_US NOW_UNIX_US
#define PREV_ANCHOR_UPTIME_US 10000000ULL

static void test_boot_seq_costs_nothing_in_the_entry(void)
{
    /* boot_seq has to be free, or persisting the ring would cost RAM per entry on every
     * build including the ones that never reboot into a recovery. It lands in padding the
     * struct already carried between trace_id and uptime_us. */
    TEST_ASSERT_EQUAL_size_t(136, sizeof(sentry_log_entry_t));
}

static void test_begin_boot_marks_what_survived_and_advances_the_counter(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "before the crash", false);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 2000, "also before", false);

    /* Nothing is recovered while the boot that wrote it is still the one running. */
    TEST_ASSERT_FALSE(sentry_log_entry_is_recovered(&r, &r.entries[0]));

    TEST_ASSERT_EQUAL_UINT(2, sentry_log_ring_begin_boot(&r));
    TEST_ASSERT_EQUAL_UINT(1, r.boot_seq);
    /* The entries are still there — surviving the reset is the whole point — but they now
     * belong to a boot that is no longer running. */
    TEST_ASSERT_EQUAL_UINT(2, r.count);
    TEST_ASSERT_TRUE(sentry_log_entry_is_recovered(&r, &r.entries[0]));
    TEST_ASSERT_TRUE(sentry_log_entry_is_recovered(&r, &r.entries[1]));

    /* A line recorded now belongs to this boot, in the same ring as the recovered ones. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 500, "after the reboot", false);
    TEST_ASSERT_FALSE(sentry_log_entry_is_recovered(&r, &r.entries[2]));
}

static void test_clear_keeps_what_describes_the_boot_not_the_batch(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_set_anchor(&r, PREV_ANCHOR_UNIX_US, PREV_ANCHOR_UPTIME_US);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "sent", false);
    sentry_log_ring_begin_boot(&r);

    sentry_log_ring_clear(&r);

    /* Contents gone... */
    TEST_ASSERT_TRUE(sentry_log_ring_empty(&r));
    /* ...but not the device's sense of which boot it is on or what time it is. A persistent
     * ring that forgot these on every successful flush could not date the next line. */
    TEST_ASSERT_EQUAL_UINT(1, r.boot_seq);
    TEST_ASSERT_EQUAL_UINT64(PREV_ANCHOR_UNIX_US, r.prev_anchor_unix_us);
    TEST_ASSERT_EQUAL_UINT64(PREV_ANCHOR_UPTIME_US, r.prev_anchor_uptime_us);
}

static void test_reset_is_the_initialiser_and_forgets_everything(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_set_anchor(&r, PREV_ANCHOR_UNIX_US, PREV_ANCHOR_UPTIME_US);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x", false);
    sentry_log_ring_begin_boot(&r);

    /* The counterpart to clear(): this is what a caller uses to establish a ring in the
     * first place, and what a persistent ring falls back to when its stored contents cannot
     * be trusted. Leaving boot_seq or an anchor behind there would date the next line
     * against a boot that never happened. */
    sentry_log_ring_reset(&r);
    TEST_ASSERT_TRUE(sentry_log_ring_empty(&r));
    TEST_ASSERT_EQUAL_UINT(0, r.boot_seq);
    TEST_ASSERT_EQUAL_UINT64(0, r.anchor_unix_us);
    TEST_ASSERT_EQUAL_UINT64(0, r.prev_anchor_unix_us);
    TEST_ASSERT_EQUAL_UINT64(0, r.prev_anchor_uptime_us);
}

/*
 * The regression this whole mechanism exists for.
 *
 * Uptime restarts at zero on reboot. Dating a recovered entry the ordinary way subtracts its
 * (large, previous-boot) uptime from this boot's (tiny) one, underflows, clamps, and stamps
 * every recovered line at the flush instant — collapsing the entire pre-crash timeline onto
 * a single point, which is precisely the thing worth keeping.
 */
static void test_a_recovered_line_is_dated_against_the_boot_that_recorded_it(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);

    /* Previous boot: clock known at 10 s of uptime, line recorded at 12 s — so two seconds
     * after the anchor, whatever this boot's clock says later. */
    sentry_log_ring_set_anchor(&r, PREV_ANCHOR_UNIX_US, PREV_ANCHOR_UPTIME_US);
    sentry_log_ring_push(&r, SENTRY_LEVEL_ERROR, TRACE, 12000000, "heap 8192, falling", false);
    sentry_log_ring_begin_boot(&r);

    /* This boot: flushing a minute later, half a second in. */
    const uint64_t now_unix = NOW_UNIX_US + 60000000ULL;
    size_t len = sentry_log_envelope_write(
        buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 500000, now_unix);
    TEST_ASSERT_TRUE(len > 0);

    /* Anchor + 2 s, on the previous boot's timeline. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561602.000000"));
    /* And emphatically not the flush instant, which is what the old derivation produced. */
    TEST_ASSERT_NULL(strstr(buf, "\"timestamp\":1755561660.000000"));
    TEST_ASSERT_NULL(strstr(buf, "\"timestamp\":1755561659.500000"));
}

static void test_recovered_lines_keep_their_order_and_spacing(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);

    sentry_log_ring_set_anchor(&r, PREV_ANCHOR_UNIX_US, PREV_ANCHOR_UPTIME_US);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 11000000, "wifi disconnected", false);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 13000000, "reconnect failed", false);
    sentry_log_ring_begin_boot(&r);

    TEST_ASSERT_TRUE(sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID,
                         500000, NOW_UNIX_US + 60000000ULL)
        > 0);

    /* A timeline, not a pile: one second and three seconds past the anchor, in that order. */
    const char *first = strstr(buf, "\"timestamp\":1755561601.000000");
    const char *second = strstr(buf, "\"timestamp\":1755561603.000000");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first < second);
}

static void test_a_recovered_line_from_a_clockless_boot_is_dated_before_this_boot(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);

    /* No set_anchor: that boot crashed before it was ever told the date, which is the
     * normal state of a device that panics early. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_FATAL, TRACE, 12000000, "about to die", false);
    sentry_log_ring_begin_boot(&r);

    TEST_ASSERT_TRUE(sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID,
                         500000, NOW_UNIX_US + 60000000ULL)
        > 0);

    /* Stamped at this boot's start instant. Wrong, but wrong in the only direction that
     * cannot mislead: the line really did happen before this boot began. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561659.500000"));
}

static void test_a_batch_may_mix_recovered_and_current_lines(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);

    sentry_log_ring_set_anchor(&r, PREV_ANCHOR_UNIX_US, PREV_ANCHOR_UPTIME_US);
    sentry_log_ring_push(&r, SENTRY_LEVEL_ERROR, TRACE, 12000000, "before the crash", false);
    sentry_log_ring_begin_boot(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 400000, "after the reboot", false);

    const uint64_t now_unix = NOW_UNIX_US + 60000000ULL;
    TEST_ASSERT_TRUE(
        sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 500000, now_unix)
        > 0);

    /* Each line dated on its own boot's timeline, in one envelope: the recovered one two
     * seconds past the old anchor, the new one 100 ms before this flush. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561602.000000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561659.900000"));
}

static void test_the_layout_id_is_stable_and_not_trivial(void)
{
    /* Read back by whatever firmware boots next, which after an OTA need not be the firmware
     * that wrote the ring. Same build must agree with itself... */
    TEST_ASSERT_EQUAL_UINT32(sentry_log_ring_layout_id(), sentry_log_ring_layout_id());
    /* ...and the value must not be a degenerate constant that would validate anything. */
    TEST_ASSERT_NOT_EQUAL_UINT32(0, sentry_log_ring_layout_id());
    TEST_ASSERT_NOT_EQUAL_UINT32(0xffffffffu, sentry_log_ring_layout_id());
}

static void test_an_overlong_body_is_truncated_not_dropped(void)
{
    sentry_log_ring_t r;
    char long_body[200];
    sentry_log_ring_reset(&r);

    memset(long_body, 'x', sizeof(long_body) - 1);
    long_body[sizeof(long_body) - 1] = '\0';
    /* Caller passes false — it never called vsnprintf and has no idea this was too long —
     * but the ring's own copy has to truncate it regardless, and that has to be visible on
     * the entry even though nobody upstream reported it. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, long_body, false);

    TEST_ASSERT_EQUAL_UINT(1, r.count);
    /* Truncated to fit, not rejected: a shortened line the operator can still read beats
     * losing it entirely. */
    TEST_ASSERT_EQUAL_UINT(SENTRY_MICRO_LOG_BODY_LEN - 1, strlen(r.entries[0].body));
    TEST_ASSERT_TRUE(r.entries[0].truncated);
}

static void test_recorded_trace_id_wins_over_the_fallback(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "during a real operation", false);

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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, "", 1000, "while idle", false);

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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "during the operation", false);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, "", 2000, "after it ended", false);

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

    sentry_log_ring_push(&r, SENTRY_LEVEL_WARNING, TRACE, 1000, "reconnect attempt 3", false);

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
    /* Not truncated, so `t7d` is omitted entirely rather than written as false. */
    TEST_ASSERT_NULL(strstr(buf, "\"t7d\""));
    TEST_ASSERT_NOT_NULL(
        strstr(buf, "\"attributes\":{\"d_id\":{\"value\":\"" DEVICE_ID "\",\"type\":\"string\"}}"));

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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 5000, "x", false);
    TEST_ASSERT_TRUE(sentry_log_envelope_write(
                         buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 1000, NOW_UNIX_US)
        > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561600.000000"));

    /* An elapsed uptime longer than the wall clock itself (unreachable in practice — no
     * device has decades of continuous uptime — but the arithmetic must not wrap) clamps to
     * the flush's own anchor instead of underflowing past it. */
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 0, "x", false);
    TEST_ASSERT_TRUE(sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID,
                         NOW_UNIX_US + 1000000, NOW_UNIX_US)
        > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1755561600.000000"));
}

static void test_envelope_write_reports_the_size_it_needs(void)
{
    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "boot complete", false);

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
        sentry_log_ring_push(&r, cases[i].level, TRACE, 1000, "x", false);

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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x", false);

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
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x", false);

    TEST_ASSERT_EQUAL_UINT(
        0, sentry_log_envelope_write(buf, sizeof(buf), &r, "", DEVICE_ID, 2000, NOW_UNIX_US));
    TEST_ASSERT_EQUAL_UINT(
        0, sentry_log_envelope_write(buf, sizeof(buf), &r, NULL, DEVICE_ID, 2000, NOW_UNIX_US));
}

static void test_a_missing_device_id_omits_it_not_the_attributes(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);
    /* truncated=true so `attributes` has something to say regardless of device_id — this
     * isolates what omitting the device_id specifically does, from whether `attributes`
     * appears at all. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x", true);

    size_t len
        = sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, NULL, 2000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"attributes\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t7d\""));
    TEST_ASSERT_NULL(strstr(buf, "\"d_id\""));
}

static void test_attributes_is_omitted_when_nothing_applies(void)
{
    sentry_log_ring_t r;
    char buf[2048];
    sentry_log_ring_reset(&r);
    /* Neither truncated nor a device_id to report — the whole key is skipped rather than
     * written as an empty object, since an envelope's worth of these is billed against the
     * same budget a full ring of long lines is already close to. */
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "x", false);

    size_t len
        = sentry_log_envelope_write(buf, sizeof(buf), &r, FALLBACK_TRACE, NULL, 2000, NOW_UNIX_US);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NULL(strstr(buf, "\"attributes\""));
}

static void test_the_t7d_attribute_reflects_what_actually_happened(void)
{
    sentry_log_ring_t r;
    char buf[2048];

    /* A line the caller knows fit fine: t7d is omitted, not written as false, since the
     * common case should not spend bytes saying nothing went wrong. */
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "short", false);
    TEST_ASSERT_TRUE(sentry_log_envelope_write(
                         buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US)
        > 0);
    TEST_ASSERT_NULL(strstr(buf, "\"t7d\""));

    /* A line the caller already knows was cut short upstream — e.g. sentry_log()'s own
     * vsnprintf() reported it would have been longer than the buffer it formatted into. */
    sentry_log_ring_reset(&r);
    sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, "short", true);
    TEST_ASSERT_TRUE(sentry_log_envelope_write(
                         buf, sizeof(buf), &r, FALLBACK_TRACE, DEVICE_ID, 2000, NOW_UNIX_US)
        > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t7d\":{\"value\":true,\"type\":\"boolean\"}"));
}

static void test_null_is_safe(void)
{
    TEST_ASSERT_TRUE(sentry_log_ring_empty(NULL));
    sentry_log_ring_reset(NULL);
    TEST_ASSERT_FALSE(sentry_log_ring_push(NULL, SENTRY_LEVEL_INFO, TRACE, 1000, "x", false));

    sentry_log_ring_t r;
    sentry_log_ring_reset(&r);
    TEST_ASSERT_FALSE(sentry_log_ring_push(&r, SENTRY_LEVEL_INFO, TRACE, 1000, NULL, false));
    TEST_ASSERT_TRUE(sentry_log_ring_empty(&r));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_line_is_recorded);
    RUN_TEST(test_lines_are_not_aggregated);
    RUN_TEST(test_a_full_ring_evicts_the_oldest_not_the_newest);
    RUN_TEST(test_entries_serialise_oldest_first);
    RUN_TEST(test_a_full_ring_of_worst_case_entries_fits_the_envelope_budget);
    RUN_TEST(test_escaping_counts_toward_the_envelope_budget);
    RUN_TEST(test_boot_seq_costs_nothing_in_the_entry);
    RUN_TEST(test_begin_boot_marks_what_survived_and_advances_the_counter);
    RUN_TEST(test_clear_keeps_what_describes_the_boot_not_the_batch);
    RUN_TEST(test_reset_is_the_initialiser_and_forgets_everything);
    RUN_TEST(test_a_recovered_line_is_dated_against_the_boot_that_recorded_it);
    RUN_TEST(test_recovered_lines_keep_their_order_and_spacing);
    RUN_TEST(test_a_recovered_line_from_a_clockless_boot_is_dated_before_this_boot);
    RUN_TEST(test_a_batch_may_mix_recovered_and_current_lines);
    RUN_TEST(test_the_layout_id_is_stable_and_not_trivial);
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
    RUN_TEST(test_a_missing_device_id_omits_it_not_the_attributes);
    RUN_TEST(test_attributes_is_omitted_when_nothing_applies);
    RUN_TEST(test_the_t7d_attribute_reflects_what_actually_happened);
    RUN_TEST(test_null_is_safe);
    return UNITY_END();
}
