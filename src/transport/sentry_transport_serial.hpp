/**
 * Serial relay transport — deliver through a host on the other end of the USB cable.
 *
 * The device has no internet; the laptop it is already plugged into does. This hands the
 * whole request over the serial link and lets a small script on the host perform it:
 *
 *     device                                     host (scripts/serial_relay.py)
 *     ------                                     -----------------------------
 *     @SENTRY-RELAY/1 <url> <auth> <ct> <body>   -> decode, check host, POST to Sentry
 *     @SENTRY-RELAY-RESULT/1 200 0               <- HTTP status and any retry-after
 *
 * It is worth more than bench convenience. This is the same shape as the BLE relay the
 * proposal describes — the device builds a complete Sentry request and something else
 * moves the bytes — so it validates that design over a link that is trivial to debug,
 * before any of it depends on a phone. The host script knows nothing about Sentry beyond
 * "POST these bytes to this URL".
 *
 * Every field is base64 so the line-framed protocol needs no escaping: URLs and auth
 * headers contain spaces, and an envelope is ndjson that *contains newlines*. It shares
 * the port with ordinary log output, which is why requests carry a distinctive prefix.
 */
#ifndef SENTRY_MICRO_TRANSPORT_SERIAL_HPP_INCLUDED
#define SENTRY_MICRO_TRANSPORT_SERIAL_HPP_INCLUDED

#include "sentry_transport.hpp"

#if defined(ARDUINO)

#    include <Arduino.h>

namespace sentry {

/** Protocol markers, shared with `scripts/serial_relay.py`. Bump the version together. */
#    define SENTRY_RELAY_REQUEST_PREFIX "@SENTRY-RELAY/1 "
#    define SENTRY_RELAY_RESULT_PREFIX "@SENTRY-RELAY-RESULT/1 "

class SerialTransport : public Transport {
public:
    /**
     * @param stream       Where to write requests. Defaults to `Serial` — the same port
     *                     the logs go to, which is the point: one cable, no extra wiring.
     * @param timeout_ms   How long to wait for the host to answer. Generous by default
     *                     because it covers the host's own DNS, TLS and round trip.
     */
    explicit SerialTransport(Stream &stream = Serial, uint32_t timeout_ms = 15000)
        : stream_(&stream)
        , timeout_ms_(timeout_ms)
    {
    }

    Response send(
        const char *url, const Headers &headers, const uint8_t *body, size_t len) override;

    /**
     * Always true.
     *
     * There is no way to detect a listener on a UART — nothing distinguishes "no script
     * running" from "script is thinking". An unanswered request times out into
     * `SEND_UNAVAILABLE`, which is the same signal the caller would act on anyway; the
     * only cost of guessing wrong is the wait.
     */
    bool is_available() override { return true; }

    const char *name() const override { return "serial"; }

    /**
     * Largest envelope this transport will relay, in bytes.
     *
     * A sanity bound, not a buffer size — encoding is streamed in fixed chunks, so memory
     * use does not grow with the envelope. It exists so a corrupted length cannot make the
     * device spend minutes shovelling garbage down a 115200-baud link.
     *
     * An enum rather than a `static const` member so that using it never requires an
     * out-of-line definition.
     */
    enum : size_t { MAX_ENVELOPE_BYTES = 4096 };

private:
    Stream *stream_;
    uint32_t timeout_ms_;
};

} // namespace sentry

#endif /* ARDUINO */

#endif /* SENTRY_MICRO_TRANSPORT_SERIAL_HPP_INCLUDED */
