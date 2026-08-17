/**
 * Host tests for the relay wire protocol.
 *
 * Framing bugs here are silent and expensive: an off-by-one in an offset produces a request
 * the host reassembles with a hole in it, which ingest answers with a 400 hours later and
 * miles from the cause. So the interesting case is not "does one frame encode" but "does a
 * request chunked at an awkward size come back out byte-for-byte" — including a chunk
 * boundary that falls in the middle of a field, which is the whole reason the four fields
 * are concatenated into one stream rather than framed individually.
 *
 * The reassembly below is deliberately written the way a companion app would write it, from
 * the header docs alone: it doubles as an executable specification of what an app has to do.
 */

#include <string.h>
#include <unity.h>

#include "sentry_relay.h"

#define URL "https://o1.ingest.us.sentry.io/api/42/envelope/"
#define AUTH "Sentry sentry_key=abc123, sentry_version=7"
#define CTYPE "application/x-sentry-envelope"

static const uint8_t BODY[] = "{\"event_id\":\"0123456789abcdef0123456789abcdef\"}\n"
                              "{\"type\":\"event\",\"length\":9}\n"
                              "{\"a\":123}\n";

static sentry_relay_request_t make_request(void)
{
    sentry_relay_request_t request;
    request.url = URL;
    request.auth = AUTH;
    request.content_type = CTYPE;
    request.body = BODY;
    request.body_len = sizeof(BODY) - 1;
    return request;
}

void setUp(void) { }
void tearDown(void) { }

static void test_stream_len_is_the_four_fields(void)
{
    const sentry_relay_request_t request = make_request();
    const size_t expected = strlen(URL) + strlen(AUTH) + strlen(CTYPE) + (size_t)(sizeof(BODY) - 1);
    TEST_ASSERT_EQUAL_size_t(expected, sentry_relay_stream_len(&request));
}

static void test_stream_read_spans_field_boundaries(void)
{
    const sentry_relay_request_t request = make_request();

    /* A read starting inside the URL and ending inside the auth header — the case that
     * exists precisely because the encoder must not care where fields end. */
    uint8_t out[64];
    const size_t offset = strlen(URL) - 4;
    const size_t got = sentry_relay_stream_read(&request, offset, out, 8);
    TEST_ASSERT_EQUAL_size_t(8, got);
    TEST_ASSERT_EQUAL_MEMORY(&URL[strlen(URL) - 4], out, 4);
    TEST_ASSERT_EQUAL_MEMORY(AUTH, out + 4, 4);

    /* Reading past the end yields nothing rather than garbage. */
    TEST_ASSERT_EQUAL_size_t(
        0, sentry_relay_stream_read(&request, sentry_relay_stream_len(&request), out, sizeof(out)));
}

static void test_begin_frame_carries_the_four_lengths(void)
{
    const sentry_relay_request_t request = make_request();
    uint8_t frame[SENTRY_RELAY_BEGIN_LEN];

    TEST_ASSERT_EQUAL_size_t(
        SENTRY_RELAY_BEGIN_LEN, sentry_relay_encode_begin(frame, sizeof(frame), 7, &request));
    TEST_ASSERT_EQUAL_UINT8(SENTRY_RELAY_FRAME_BEGIN, frame[0]);
    TEST_ASSERT_EQUAL_UINT8(7, frame[1]);
    TEST_ASSERT_EQUAL_UINT16(strlen(URL), (uint16_t)(frame[2] | (frame[3] << 8)));
    TEST_ASSERT_EQUAL_UINT16(strlen(AUTH), (uint16_t)(frame[4] | (frame[5] << 8)));
    TEST_ASSERT_EQUAL_UINT16(strlen(CTYPE), (uint16_t)(frame[6] | (frame[7] << 8)));
    TEST_ASSERT_EQUAL_UINT32(sizeof(BODY) - 1,
        (uint32_t)frame[8] | ((uint32_t)frame[9] << 8) | ((uint32_t)frame[10] << 16)
            | ((uint32_t)frame[11] << 24));

    /* Too small a buffer writes nothing at all rather than a partial header. */
    TEST_ASSERT_EQUAL_size_t(
        0, sentry_relay_encode_begin(frame, SENTRY_RELAY_BEGIN_LEN - 1, 7, &request));
}

/** Reassemble a chunked request the way a companion app has to, then check it round-trips. */
static void reassemble_at_chunk_size(size_t chunk_bytes)
{
    const sentry_relay_request_t request = make_request();

    uint8_t frame[512];
    size_t frame_len = sentry_relay_encode_begin(frame, sizeof(frame), 3, &request);
    TEST_ASSERT_EQUAL_size_t(SENTRY_RELAY_BEGIN_LEN, frame_len);

    const uint16_t url_len = (uint16_t)(frame[2] | (frame[3] << 8));
    const uint16_t auth_len = (uint16_t)(frame[4] | (frame[5] << 8));
    const uint16_t ctype_len = (uint16_t)(frame[6] | (frame[7] << 8));
    const uint32_t body_len = (uint32_t)frame[8] | ((uint32_t)frame[9] << 8)
        | ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 24);
    const size_t total = (size_t)url_len + auth_len + ctype_len + body_len;

    static uint8_t assembled[1024];
    memset(assembled, 0, sizeof(assembled));
    TEST_ASSERT_TRUE(total <= sizeof(assembled));

    size_t received = 0;
    size_t offset = 0;
    size_t frames = 0;
    while (offset < total) {
        frame_len = sentry_relay_encode_data(frame, chunk_bytes, 3, &request, offset);
        TEST_ASSERT_TRUE(frame_len > SENTRY_RELAY_DATA_HEADER_LEN);
        TEST_ASSERT_TRUE(frame_len <= chunk_bytes);
        TEST_ASSERT_EQUAL_UINT8(SENTRY_RELAY_FRAME_DATA, frame[0]);
        TEST_ASSERT_EQUAL_UINT8(3, frame[1]);

        const uint16_t at = (uint16_t)(frame[2] | (frame[3] << 8));
        const size_t payload = frame_len - SENTRY_RELAY_DATA_HEADER_LEN;
        memcpy(assembled + at, frame + SENTRY_RELAY_DATA_HEADER_LEN, payload);
        received += payload;
        offset += payload;
        frames++;
        TEST_ASSERT_TRUE(frames < 500); /* never loop forever on a stalled encoder */
    }
    TEST_ASSERT_EQUAL_size_t(total, received);

    /* And the app slices it back into the request it has to perform. */
    TEST_ASSERT_EQUAL_size_t(strlen(URL), url_len);
    TEST_ASSERT_EQUAL_MEMORY(URL, assembled, url_len);
    TEST_ASSERT_EQUAL_MEMORY(AUTH, assembled + url_len, auth_len);
    TEST_ASSERT_EQUAL_MEMORY(CTYPE, assembled + url_len + auth_len, ctype_len);
    TEST_ASSERT_EQUAL_MEMORY(BODY, assembled + url_len + auth_len + ctype_len, body_len);

    /* Past the end, the encoder reports exhaustion rather than emitting an empty frame. */
    TEST_ASSERT_EQUAL_size_t(0, sentry_relay_encode_data(frame, chunk_bytes, 3, &request, total));
}

static void test_round_trip_at_every_awkward_chunk_size(void)
{
    /* 24 is the protocol minimum, 180 the BLE default, and the odd sizes in between put a
     * chunk boundary inside each of the four fields in turn. */
    const size_t sizes[] = { 24, 25, 31, 47, 64, 100, 180, 512 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        reassemble_at_chunk_size(sizes[i]);
    }
}

static void test_end_frame(void)
{
    uint8_t frame[SENTRY_RELAY_END_LEN];
    TEST_ASSERT_EQUAL_size_t(
        SENTRY_RELAY_END_LEN, sentry_relay_encode_end(frame, sizeof(frame), 9));
    TEST_ASSERT_EQUAL_UINT8(SENTRY_RELAY_FRAME_END, frame[0]);
    TEST_ASSERT_EQUAL_UINT8(9, frame[1]);
    TEST_ASSERT_EQUAL_size_t(0, sentry_relay_encode_end(frame, 1, 9));
}

static void test_parse_hello(void)
{
    const uint8_t hello[] = { SENTRY_RELAY_FRAME_HELLO, SENTRY_RELAY_PROTOCOL_VERSION, 0xB4, 0x00 };
    sentry_relay_host_frame_t frame;
    TEST_ASSERT_TRUE(sentry_relay_parse_host_frame(&frame, hello, sizeof(hello)));
    TEST_ASSERT_EQUAL_INT(SENTRY_RELAY_HOST_HELLO, frame.kind);
    TEST_ASSERT_EQUAL_UINT8(SENTRY_RELAY_PROTOCOL_VERSION, frame.protocol_version);
    TEST_ASSERT_EQUAL_UINT16(180, frame.max_chunk_bytes);
}

static void test_parse_status(void)
{
    /* rate limited, HTTP 429, retry after 60000 ms */
    const uint8_t status[] = { SENTRY_RELAY_FRAME_STATUS, 5, SENTRY_RELAY_RESULT_RATE_LIMITED, 0xAD,
        0x01, 0x60, 0xEA, 0x00, 0x00 };
    sentry_relay_host_frame_t frame;
    TEST_ASSERT_TRUE(sentry_relay_parse_host_frame(&frame, status, sizeof(status)));
    TEST_ASSERT_EQUAL_INT(SENTRY_RELAY_HOST_STATUS, frame.kind);
    TEST_ASSERT_EQUAL_UINT8(5, frame.request_id);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_RATE_LIMITED, frame.response.result);
    TEST_ASSERT_EQUAL_UINT16(429, frame.response.http_status);
    TEST_ASSERT_EQUAL_UINT32(60000, frame.response.retry_after_ms);
}

static void test_parse_rejects_junk(void)
{
    sentry_relay_host_frame_t frame;
    const uint8_t truncated[] = { SENTRY_RELAY_FRAME_STATUS, 1, 0 };
    const uint8_t unknown[] = { 0x7F, 1, 2, 3, 4, 5, 6, 7, 8 };

    TEST_ASSERT_FALSE(sentry_relay_parse_host_frame(&frame, NULL, 0));
    TEST_ASSERT_EQUAL_INT(SENTRY_RELAY_HOST_NONE, frame.kind);
    TEST_ASSERT_FALSE(sentry_relay_parse_host_frame(&frame, truncated, sizeof(truncated)));
    TEST_ASSERT_EQUAL_INT(SENTRY_RELAY_HOST_NONE, frame.kind);
    TEST_ASSERT_FALSE(sentry_relay_parse_host_frame(&frame, unknown, sizeof(unknown)));
    TEST_ASSERT_EQUAL_INT(SENTRY_RELAY_HOST_NONE, frame.kind);
}

static void test_unknown_result_code_is_retryable(void)
{
    /* A host from the future reporting an outcome this build has never heard of must not be
     * read as success — that would drop the event on the floor. */
    const uint8_t status[] = { SENTRY_RELAY_FRAME_STATUS, 1, 99, 0, 0, 0, 0, 0, 0 };
    sentry_relay_host_frame_t frame;
    TEST_ASSERT_TRUE(sentry_relay_parse_host_frame(&frame, status, sizeof(status)));
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, frame.response.result);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stream_len_is_the_four_fields);
    RUN_TEST(test_stream_read_spans_field_boundaries);
    RUN_TEST(test_begin_frame_carries_the_four_lengths);
    RUN_TEST(test_round_trip_at_every_awkward_chunk_size);
    RUN_TEST(test_end_frame);
    RUN_TEST(test_parse_hello);
    RUN_TEST(test_parse_status);
    RUN_TEST(test_parse_rejects_junk);
    RUN_TEST(test_unknown_result_code_is_retryable);
    return UNITY_END();
}
