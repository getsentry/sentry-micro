#include "sentry_relay.h"

#include <string.h>

/* The four fields, in the order they appear in the logical stream. Keeping the layout in one
 * table rather than four copies of the same arithmetic is what makes `stream_read` a loop
 * instead of a nest of boundary conditions — and boundary conditions between fields are
 * exactly where a chunked encoder goes wrong. */
typedef struct {
    const uint8_t *data;
    size_t len;
} segment_t;

static size_t str_len(const char *s) { return s ? strlen(s) : 0; }

static void segments_of(const sentry_relay_request_t *request, segment_t out[4])
{
    out[0].data = (const uint8_t *)(request->url ? request->url : "");
    out[0].len = str_len(request->url);
    out[1].data = (const uint8_t *)(request->auth ? request->auth : "");
    out[1].len = str_len(request->auth);
    out[2].data = (const uint8_t *)(request->content_type ? request->content_type : "");
    out[2].len = str_len(request->content_type);
    out[3].data = request->body;
    out[3].len = request->body ? request->body_len : 0;
}

static void write_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
    out[2] = (uint8_t)((value >> 16) & 0xFF);
    out[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint16_t read_u16(const uint8_t *in) { return (uint16_t)(in[0] | ((uint16_t)in[1] << 8)); }

static uint32_t read_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
        | ((uint32_t)in[3] << 24);
}

size_t sentry_relay_stream_len(const sentry_relay_request_t *request)
{
    if (!request) {
        return 0;
    }
    segment_t segments[4];
    segments_of(request, segments);
    return segments[0].len + segments[1].len + segments[2].len + segments[3].len;
}

size_t sentry_relay_stream_read(
    const sentry_relay_request_t *request, size_t offset, uint8_t *out, size_t cap)
{
    if (!request || !out || cap == 0) {
        return 0;
    }

    segment_t segments[4];
    segments_of(request, segments);

    size_t written = 0;
    size_t base = 0;
    for (int i = 0; i < 4 && written < cap; i++) {
        const size_t len = segments[i].len;
        if (offset >= base + len) {
            base += len;
            continue; /* entirely before the read */
        }
        const size_t start = (offset > base) ? offset - base : 0;
        size_t take = len - start;
        if (take > cap - written) {
            take = cap - written;
        }
        if (segments[i].data && take > 0) {
            memcpy(out + written, segments[i].data + start, take);
        }
        written += take;
        base += len;
    }
    return written;
}

size_t sentry_relay_encode_begin(
    uint8_t *out, size_t cap, uint8_t request_id, const sentry_relay_request_t *request)
{
    if (!out || !request || cap < SENTRY_RELAY_BEGIN_LEN) {
        return 0;
    }

    const size_t url_len = str_len(request->url);
    const size_t auth_len = str_len(request->auth);
    const size_t ctype_len = str_len(request->content_type);
    const size_t body_len = request->body ? request->body_len : 0;

    /* Offsets are 16-bit on the wire, so the whole stream has to be addressable by one.
     * Refusing here is better than silently wrapping an offset and having the host
     * reassemble a request with a hole in it. */
    if (sentry_relay_stream_len(request) > 0xFFFF) {
        return 0;
    }
    if (url_len > 0xFFFF || auth_len > 0xFFFF || ctype_len > 0xFFFF) {
        return 0;
    }

    out[0] = SENTRY_RELAY_FRAME_BEGIN;
    out[1] = request_id;
    write_u16(out + 2, (uint16_t)url_len);
    write_u16(out + 4, (uint16_t)auth_len);
    write_u16(out + 6, (uint16_t)ctype_len);
    write_u32(out + 8, (uint32_t)body_len);
    return SENTRY_RELAY_BEGIN_LEN;
}

size_t sentry_relay_encode_data(uint8_t *out, size_t cap, uint8_t request_id,
    const sentry_relay_request_t *request, size_t offset)
{
    if (!out || !request || cap <= SENTRY_RELAY_DATA_HEADER_LEN || offset > 0xFFFF) {
        return 0;
    }

    const size_t payload = sentry_relay_stream_read(
        request, offset, out + SENTRY_RELAY_DATA_HEADER_LEN, cap - SENTRY_RELAY_DATA_HEADER_LEN);
    if (payload == 0) {
        return 0; /* stream exhausted */
    }

    out[0] = SENTRY_RELAY_FRAME_DATA;
    out[1] = request_id;
    write_u16(out + 2, (uint16_t)offset);
    return SENTRY_RELAY_DATA_HEADER_LEN + payload;
}

size_t sentry_relay_encode_end(uint8_t *out, size_t cap, uint8_t request_id)
{
    if (!out || cap < SENTRY_RELAY_END_LEN) {
        return 0;
    }
    out[0] = SENTRY_RELAY_FRAME_END;
    out[1] = request_id;
    return SENTRY_RELAY_END_LEN;
}

static sentry_send_result_t result_from_wire(uint8_t code)
{
    switch (code) {
    case SENTRY_RELAY_RESULT_OK:
        return SENTRY_SEND_OK;
    case SENTRY_RELAY_RESULT_UNAVAILABLE:
        return SENTRY_SEND_UNAVAILABLE;
    case SENTRY_RELAY_RESULT_REJECTED:
        return SENTRY_SEND_REJECTED;
    case SENTRY_RELAY_RESULT_RATE_LIMITED:
        return SENTRY_SEND_RATE_LIMITED;
    default:
        /* Includes SENTRY_RELAY_RESULT_ERROR and anything a future host invents: treat an
         * unrecognised outcome as a retryable error rather than as success. */
        return SENTRY_SEND_ERROR;
    }
}

bool sentry_relay_parse_host_frame(sentry_relay_host_frame_t *out, const uint8_t *data, size_t len)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->response = sentry_response_make(SENTRY_SEND_ERROR);

    if (!data || len < 1) {
        return false;
    }

    switch (data[0]) {
    case SENTRY_RELAY_FRAME_HELLO:
        if (len < SENTRY_RELAY_HELLO_LEN) {
            return false;
        }
        out->kind = SENTRY_RELAY_HOST_HELLO;
        out->protocol_version = data[1];
        out->max_chunk_bytes = read_u16(data + 2);
        return true;

    case SENTRY_RELAY_FRAME_STATUS:
        if (len < SENTRY_RELAY_STATUS_LEN) {
            return false;
        }
        out->kind = SENTRY_RELAY_HOST_STATUS;
        out->request_id = data[1];
        out->response.result = result_from_wire(data[2]);
        out->response.http_status = read_u16(data + 3);
        out->response.retry_after_ms = read_u32(data + 5);
        return true;

    default:
        return false;
    }
}
