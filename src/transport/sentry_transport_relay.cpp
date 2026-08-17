#include "sentry_transport_relay.hpp"

#include "../sentry_micro.h"

#if defined(ARDUINO)
#    include <Arduino.h>
#endif

namespace sentry {

namespace {

#if defined(ARDUINO)
void arduino_wait(void *, uint32_t ms) { delay(ms); }
uint32_t arduino_clock(void *) { return (uint32_t)millis(); }
#endif

/* Enough for the largest frame we emit. BEGIN and END are tiny; DATA is header plus
 * whatever the link's chunk size allows, and this bounds that. It lives on the stack of
 * send() rather than in the object because the transport spends almost all of its life
 * idle, and 256 bytes of permanently-resident RAM on a chip with 40 KB free is worth
 * more than the tidiness. */
const size_t FRAME_BUFFER_BYTES = 256;

/* How long each yield lasts while waiting for the host. Short enough that a fast host
 * is not held up by the polling granularity, long enough that the wait costs a
 * negligible number of context switches over a multi-second HTTP request. */
const uint32_t POLL_INTERVAL_MS = 5;

} // namespace

RelayTransport::RelayTransport(WriteFn write, void *ctx)
    : write_(write)
#if defined(ARDUINO)
    , wait_(&arduino_wait)
    , clock_(&arduino_clock)
#else
    , wait_(nullptr)
    , clock_(nullptr)
#endif
    , ctx_(ctx)
{
    status_response_ = sentry_response_make(SENTRY_SEND_ERROR);
}

void RelayTransport::set_chunk_bytes(size_t bytes)
{
    if (bytes < SENTRY_RELAY_MIN_CHUNK_BYTES) {
        bytes = SENTRY_RELAY_MIN_CHUNK_BYTES;
    }
    if (bytes > FRAME_BUFFER_BYTES) {
        bytes = FRAME_BUFFER_BYTES;
    }
    configured_chunk_bytes_ = bytes;
    chunk_bytes_ = bytes;
}

void RelayTransport::set_host_attached(bool attached)
{
    attached_.store(attached, std::memory_order_relaxed);
    if (!attached) {
        /* A new host is a new conversation: it has to say hello before we will send it
         * anything, and any answer still owed by the old one is void. */
        hello_seen_.store(false, std::memory_order_relaxed);
        status_pending_.store(false, std::memory_order_relaxed);
        chunk_bytes_ = configured_chunk_bytes_;
    }
}

void RelayTransport::on_host_frame(const uint8_t *data, size_t len)
{
    sentry_relay_host_frame_t frame;
    if (!sentry_relay_parse_host_frame(&frame, data, len)) {
        return;
    }

    if (frame.kind == SENTRY_RELAY_HOST_HELLO) {
        /* An unknown protocol version means frames we send would be misparsed — silence is
         * the safe answer, and the events stay buffered for a host that does speak it. */
        if (frame.protocol_version != SENTRY_RELAY_PROTOCOL_VERSION) {
            hello_seen_.store(false, std::memory_order_relaxed);
            return;
        }
        size_t host_chunk = frame.max_chunk_bytes;
        chunk_bytes_ = configured_chunk_bytes_;
        if (host_chunk >= SENTRY_RELAY_MIN_CHUNK_BYTES && host_chunk < chunk_bytes_) {
            chunk_bytes_ = host_chunk;
        }
        hello_seen_.store(true, std::memory_order_release);
        return;
    }

    if (frame.kind == SENTRY_RELAY_HOST_STATUS) {
        status_response_ = frame.response;
        status_request_id_ = frame.request_id;
        /* Release: everything written above is visible to whoever observes this as true. */
        status_pending_.store(true, std::memory_order_release);
    }
}

bool RelayTransport::write_frame(const uint8_t *frame, size_t len)
{
    return write_ ? write_(ctx_, frame, len) : false;
}

Response RelayTransport::await_status(uint8_t request_id)
{
    const uint32_t started = clock_(ctx_);
    for (;;) {
        /* Acquire: pairs with the release in on_host_frame(), so the response and request
         * id read below are the ones that were published, not a partial write. */
        if (status_pending_.load(std::memory_order_acquire)) {
            status_pending_.store(false, std::memory_order_relaxed);
            /* A status for a different request is a leftover from one that timed out.
             * Discard it and keep waiting rather than reporting somebody else's outcome. */
            if (status_request_id_ == request_id) {
                return status_response_;
            }
        }
        if (!attached_.load(std::memory_order_relaxed)) {
            return SEND_UNAVAILABLE; /* the host walked away mid-request */
        }
        if ((uint32_t)(clock_(ctx_) - started) >= timeout_ms_) {
            /* Unknown, not failed: the host may well have performed the request and lost
             * the answer. SEND_ERROR keeps it buffered, and ingest deduplicates the retry
             * by event id, so the worst case is a resend rather than a lost crash. */
            return SEND_ERROR;
        }
        if (wait_) {
            wait_(ctx_, POLL_INTERVAL_MS);
        }
    }
}

Response RelayTransport::send(
    const char *url, const Headers &headers, const uint8_t *body, size_t len)
{
    /* `wait_` is as load-bearing as the clock. Without it await_status() spins at full
     * tilt for the whole timeout, starving the very task that has to deliver the answer —
     * so the wait would always time out. Refusing is the honest failure. */
    if (!write_ || !clock_ || !wait_) {
        return SEND_UNAVAILABLE;
    }
    if (!host_ready()) {
        return SEND_UNAVAILABLE;
    }

    /*
     * Refuse anything but the DSN's ingest host — the same check the WiFi and serial
     * transports make, and it matters most here.
     *
     * This transport asks *someone else's machine* to perform the request: a phone, a
     * gateway, a laptop. A device that could name an arbitrary URL would be an open proxy
     * running on its owner's hardware. The companion app must enforce this too, since it is
     * the one making the call and cannot trust the device; checking here as well means
     * neither end depends on the other having got it right.
     */
    const sentry_dsn_t *dsn = sentry_get_dsn();
    if (!url || !dsn || !dsn->valid || !sentry_url_host_matches(url, dsn->host)) {
        return SEND_REJECTED;
    }

    sentry_relay_request_t request;
    request.url = url;
    request.auth = headers.auth;
    request.content_type = headers.content_type;
    request.body = body;
    request.body_len = len;

    const uint8_t request_id = next_request_id_++;
    if (next_request_id_ == 0) {
        next_request_id_ = 1; /* 0 stays free as "no request" for hosts that want a sentinel */
    }
    status_pending_.store(false, std::memory_order_relaxed);

    uint8_t frame[FRAME_BUFFER_BYTES];
    size_t frame_len = sentry_relay_encode_begin(frame, sizeof(frame), request_id, &request);
    if (frame_len == 0) {
        /* Only reachable for a request too large for the protocol, which is a bug in the
         * caller rather than a delivery problem — retrying it would fail identically. */
        return SEND_REJECTED;
    }
    if (!write_frame(frame, frame_len)) {
        return SEND_UNAVAILABLE;
    }

    const size_t stream_len = sentry_relay_stream_len(&request);
    for (size_t offset = 0; offset < stream_len;) {
        frame_len = sentry_relay_encode_data(frame, chunk_bytes_, request_id, &request, offset);
        if (frame_len == 0) {
            return SEND_ERROR;
        }
        if (!write_frame(frame, frame_len)) {
            return SEND_UNAVAILABLE;
        }
        offset += frame_len - SENTRY_RELAY_DATA_HEADER_LEN;
    }

    frame_len = sentry_relay_encode_end(frame, sizeof(frame), request_id);
    if (frame_len == 0 || !write_frame(frame, frame_len)) {
        return SEND_UNAVAILABLE;
    }

    return await_status(request_id);
}

} // namespace sentry
