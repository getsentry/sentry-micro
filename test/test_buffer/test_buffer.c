/**
 * Host tests for the offline ring buffer.
 *
 * The buffer is the difference between "the crash was reported late" and "the crash was
 * never reported", and every bug in it is silent: a device that loses buffered events looks
 * exactly like a device that never crashed. It also has to survive reboots, which is where
 * ring-buffer bugs normally hide, so persistence and recovery are tested explicitly.
 *
 * The storage below is a plain in-memory array — the same vtable NVS implements on the
 * device, which is what lets this run in milliseconds on a laptop.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "sentry_buffer.h"

#define SLOTS 4
#define SLOT_CAP 256

typedef struct {
    uint8_t data[SLOTS][SLOT_CAP];
    size_t len[SLOTS];
    bool occupied[SLOTS];
    uint32_t head, tail, dropped;
    bool has_meta;
    /* Set to make the next write fail, standing in for a worn-out or full flash. */
    bool fail_next_write;
} fake_storage_t;

static fake_storage_t g_store;

static bool fake_write(void *ctx, uint32_t index, const uint8_t *data, size_t len)
{
    fake_storage_t *s = (fake_storage_t *)ctx;
    if (s->fail_next_write) {
        s->fail_next_write = false;
        return false;
    }
    if (index >= SLOTS || len > SLOT_CAP) {
        return false;
    }
    memcpy(s->data[index], data, len);
    s->len[index] = len;
    s->occupied[index] = true;
    return true;
}

static bool fake_read(void *ctx, uint32_t index, uint8_t *out, size_t cap, size_t *out_len)
{
    fake_storage_t *s = (fake_storage_t *)ctx;
    if (index >= SLOTS || !s->occupied[index] || s->len[index] > cap) {
        return false;
    }
    memcpy(out, s->data[index], s->len[index]);
    *out_len = s->len[index];
    return true;
}

static bool fake_erase(void *ctx, uint32_t index)
{
    fake_storage_t *s = (fake_storage_t *)ctx;
    if (index >= SLOTS) {
        return false;
    }
    s->occupied[index] = false;
    s->len[index] = 0;
    return true;
}

static bool fake_load_meta(void *ctx, uint32_t *head, uint32_t *tail, uint32_t *dropped)
{
    fake_storage_t *s = (fake_storage_t *)ctx;
    if (!s->has_meta) {
        return false;
    }
    *head = s->head;
    *tail = s->tail;
    *dropped = s->dropped;
    return true;
}

static bool fake_save_meta(void *ctx, uint32_t head, uint32_t tail, uint32_t dropped)
{
    fake_storage_t *s = (fake_storage_t *)ctx;
    s->head = head;
    s->tail = tail;
    s->dropped = dropped;
    s->has_meta = true;
    return true;
}

static sentry_storage_t make_storage(void)
{
    sentry_storage_t storage;
    memset(&storage, 0, sizeof(storage));
    storage.write = fake_write;
    storage.read = fake_read;
    storage.erase = fake_erase;
    storage.load_meta = fake_load_meta;
    storage.save_meta = fake_save_meta;
    storage.ctx = &g_store;
    storage.slot_count = SLOTS;
    return storage;
}

void setUp(void) { memset(&g_store, 0, sizeof(g_store)); }
void tearDown(void) { }

static void push_text(sentry_buffer_t *buffer, const char *text)
{
    TEST_ASSERT_TRUE_MESSAGE(sentry_buffer_push(buffer, (const uint8_t *)text, strlen(text)), text);
}

static void assert_peek_equals(sentry_buffer_t *buffer, const char *expected)
{
    uint8_t out[SLOT_CAP];
    size_t len = 0;
    TEST_ASSERT_TRUE(sentry_buffer_peek(buffer, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_STRING_LEN(expected, out, len);
}

static void test_delivers_envelopes_in_order(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    TEST_ASSERT_TRUE(sentry_buffer_init(&buffer, &storage));
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_count(&buffer));

    push_text(&buffer, "first");
    push_text(&buffer, "second");
    TEST_ASSERT_EQUAL_UINT32(2, sentry_buffer_count(&buffer));

    /* Oldest first: a crash report is only useful in the order it happened. */
    assert_peek_equals(&buffer, "first");
    TEST_ASSERT_TRUE(sentry_buffer_pop(&buffer));
    assert_peek_equals(&buffer, "second");
    TEST_ASSERT_TRUE(sentry_buffer_pop(&buffer));
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_count(&buffer));
}

static void test_peek_does_not_consume(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);
    push_text(&buffer, "only");

    /* A failed delivery must be survivable — that is the entire purpose of the buffer. */
    assert_peek_equals(&buffer, "only");
    assert_peek_equals(&buffer, "only");
    TEST_ASSERT_EQUAL_UINT32(1, sentry_buffer_count(&buffer));
}

static void test_evicts_the_oldest_when_full(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);

    push_text(&buffer, "e0");
    push_text(&buffer, "e1");
    push_text(&buffer, "e2");
    push_text(&buffer, "e3");
    TEST_ASSERT_EQUAL_UINT32(SLOTS, sentry_buffer_count(&buffer));
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_dropped(&buffer));

    push_text(&buffer, "e4");
    /* Still full, and the oldest is gone rather than the newest being refused. */
    TEST_ASSERT_EQUAL_UINT32(SLOTS, sentry_buffer_count(&buffer));
    TEST_ASSERT_EQUAL_UINT32(1, sentry_buffer_dropped(&buffer));
    assert_peek_equals(&buffer, "e1");

    push_text(&buffer, "e5");
    TEST_ASSERT_EQUAL_UINT32(2, sentry_buffer_dropped(&buffer));
    assert_peek_equals(&buffer, "e2");
}

static void test_counts_dropped_events_until_reported(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);

    for (int i = 0; i < SLOTS + 3; i++) {
        char text[16];
        snprintf(text, sizeof(text), "event-%d", i);
        push_text(&buffer, text);
    }
    /* A buffer that silently overwrites is indistinguishable from one that is working, so
     * the loss is counted and kept until something reports it. */
    TEST_ASSERT_EQUAL_UINT32(3, sentry_buffer_dropped(&buffer));
    sentry_buffer_reset_dropped(&buffer);
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_dropped(&buffer));
}

static void test_survives_a_reboot(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);
    push_text(&buffer, "before-reboot-a");
    push_text(&buffer, "before-reboot-b");

    /* Reboot: the buffer struct is gone, the storage is not. This is the case that
     * matters most — a crash report is written just before the device restarts. */
    sentry_buffer_t recovered;
    TEST_ASSERT_TRUE(sentry_buffer_init(&recovered, &storage));
    TEST_ASSERT_EQUAL_UINT32(2, sentry_buffer_count(&recovered));
    assert_peek_equals(&recovered, "before-reboot-a");
    TEST_ASSERT_TRUE(sentry_buffer_pop(&recovered));
    assert_peek_equals(&recovered, "before-reboot-b");
}

static void test_survives_a_reboot_while_wrapped(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);

    /* Drive head and tail past the end so the recovered indices are wrapped, which is
     * where an off-by-one in the count arithmetic would hide. */
    for (int i = 0; i < 6; i++) {
        char text[16];
        snprintf(text, sizeof(text), "w%d", i);
        push_text(&buffer, text);
    }
    sentry_buffer_pop(&buffer);
    uint32_t expected = sentry_buffer_count(&buffer);

    sentry_buffer_t recovered;
    TEST_ASSERT_TRUE(sentry_buffer_init(&recovered, &storage));
    TEST_ASSERT_EQUAL_UINT32(expected, sentry_buffer_count(&recovered));
    assert_peek_equals(&recovered, "w3");
}

static void test_recovers_from_corrupt_metadata(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);
    push_text(&buffer, "x");

    /* A half-finished flash write or a firmware downgrade can leave indices past the end of
     * the ring. Trusting them would index out of bounds on the first read. */
    g_store.head = 99;
    g_store.tail = 1234;

    sentry_buffer_t recovered;
    TEST_ASSERT_TRUE(sentry_buffer_init(&recovered, &storage));
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_count(&recovered));
    uint8_t out[SLOT_CAP];
    size_t len = 0;
    TEST_ASSERT_FALSE(sentry_buffer_peek(&recovered, out, sizeof(out), &len));
}

static void test_reports_a_failed_write_without_corrupting_state(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);
    push_text(&buffer, "kept");

    g_store.fail_next_write = true;
    TEST_ASSERT_FALSE(sentry_buffer_push(&buffer, (const uint8_t *)"lost", 4));

    /* The failure must not advance the ring or lose what was already buffered — flash
     * wears out, and the reporter has to degrade rather than break. */
    TEST_ASSERT_EQUAL_UINT32(1, sentry_buffer_count(&buffer));
    assert_peek_equals(&buffer, "kept");
}

static void test_rejects_unusable_storage(void)
{
    sentry_buffer_t buffer;
    sentry_storage_t incomplete = make_storage();
    incomplete.write = NULL;
    TEST_ASSERT_FALSE(sentry_buffer_init(&buffer, &incomplete));
    /* Disabled, not crashing: no buffer is worse than a buffer, but far better than a
     * reporter that faults inside a crash handler. */
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_count(&buffer));
    TEST_ASSERT_FALSE(sentry_buffer_push(&buffer, (const uint8_t *)"x", 1));
    TEST_ASSERT_FALSE(sentry_buffer_pop(&buffer));

    sentry_storage_t zero_slots = make_storage();
    zero_slots.slot_count = 0;
    TEST_ASSERT_FALSE(sentry_buffer_init(&buffer, &zero_slots));
    TEST_ASSERT_FALSE(sentry_buffer_init(&buffer, NULL));
}

static void test_clear_empties_the_buffer(void)
{
    sentry_storage_t storage = make_storage();
    sentry_buffer_t buffer;
    sentry_buffer_init(&buffer, &storage);
    push_text(&buffer, "a");
    push_text(&buffer, "b");

    sentry_buffer_clear(&buffer);
    TEST_ASSERT_EQUAL_UINT32(0, sentry_buffer_count(&buffer));

    /* And it is still usable afterwards. */
    push_text(&buffer, "c");
    assert_peek_equals(&buffer, "c");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_delivers_envelopes_in_order);
    RUN_TEST(test_peek_does_not_consume);
    RUN_TEST(test_evicts_the_oldest_when_full);
    RUN_TEST(test_counts_dropped_events_until_reported);
    RUN_TEST(test_survives_a_reboot);
    RUN_TEST(test_survives_a_reboot_while_wrapped);
    RUN_TEST(test_recovers_from_corrupt_metadata);
    RUN_TEST(test_reports_a_failed_write_without_corrupting_state);
    RUN_TEST(test_rejects_unusable_storage);
    RUN_TEST(test_clear_empties_the_buffer);
    return UNITY_END();
}
