/**
 * Relay transport — let the companion app make the request.
 *
 * The case the whole design exists for: a BLE LED controller, a LoRa sensor, a board on a
 * bench with only a USB cable. It has no route to the internet, but the thing it is talked
 * to *by* does. This transport chunks the request out over that link, waits for the host to
 * report what happened, and turns the answer back into a `Response`.
 *
 * Deliberately knows nothing about BLE, NimBLE, sockets or serial ports. It is given one
 * callback that puts a frame on the wire, and is fed the frames that come back:
 *
 *     static bool notify(void *, const uint8_t *frame, size_t len) {
 *         characteristic->setValue(frame, len);
 *         characteristic->notify();
 *         delay(8);                       // pacing belongs to the link, not to the SDK
 *         return true;
 *     }
 *
 *     static sentry::RelayTransport transport(notify);
 *     sentry::set_transport(transport);
 *
 *     // from your BLE write callback, on whatever task it runs:
 *     transport.on_host_frame(data, len);
 *     // and from your connect/disconnect handlers:
 *     transport.set_host_attached(connected);
 *
 * Any link that can carry short packets both ways can host it, and the app on the other end
 * needs no Sentry knowledge at all — see `core/sentry_relay.h` for the wire protocol.
 *
 * **`send()` blocks** until the host answers or the timeout expires, because that is the
 * contract the core's transport interface has. On a cooperative firmware that means the
 * calling task stalls for the length of one HTTP request, so call it from somewhere a pause
 * is acceptable — `sentry_flush()` on a timer, not the middle of a render loop.
 */
#ifndef SENTRY_MICRO_TRANSPORT_RELAY_HPP_INCLUDED
#define SENTRY_MICRO_TRANSPORT_RELAY_HPP_INCLUDED

#include <atomic>

#include "../core/sentry_relay.h"
#include "sentry_transport.hpp"

namespace sentry {

class RelayTransport : public Transport {
public:
    /**
     * Put one frame on the wire. Return false if it could not be sent, which aborts the
     * request and reports `SEND_UNAVAILABLE` — the "buffer it and try later" signal.
     *
     * Any pacing the link needs (BLE notification credit, serial flush) belongs here: the
     * SDK cannot know what your radio wants, and a hard-coded delay would be wrong on every
     * link but one.
     */
    typedef bool (*WriteFn)(void *ctx, const uint8_t *frame, size_t len);

    /**
     * Yield for `ms` while waiting for the host to answer.
     *
     * Required on a cooperative or single-task system, where spinning would starve the very
     * task that has to deliver the response. Defaults to Arduino's `delay()` where that
     * exists, which yields to FreeRTOS.
     */
    typedef void (*WaitFn)(void *ctx, uint32_t ms);

    /**
     * Milliseconds since boot, for the response timeout.
     *
     * Defaults to Arduino's `millis()`. A non-Arduino build must supply this and a wait
     * function; without either, the transport refuses to relay rather than spinning or
     * hanging.
     */
    typedef uint32_t (*ClockFn)(void *ctx);

    explicit RelayTransport(WriteFn write, void *ctx = nullptr);

    /** Override the default yield. */
    void set_wait_fn(WaitFn wait) { wait_ = wait; }

    /** Override the default clock. */
    void set_clock_fn(ClockFn clock) { clock_ = clock; }

    /**
     * Largest frame the link can carry, headers included. Clamped to what the host asked
     * for in HELLO when that is smaller. Default 180, which fits a BLE notification at the
     * MTU most stacks settle on without fragmenting.
     */
    void set_chunk_bytes(size_t bytes);

    /** How long to wait for a STATUS frame before giving up. Default 20s. */
    void set_timeout_ms(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }

    /**
     * Whether the link is up — a BLE central is connected, the cable is plugged in.
     *
     * Separate from HELLO: a connected host that has never said hello is one that does not
     * speak this protocol (an older app), and relaying to it would send frames into a void
     * and then block until the timeout. Both must be true before `is_available()` is.
     */
    void set_host_attached(bool attached);

    /**
     * Feed one frame received from the host.
     *
     * Safe to call from another task — the BLE stack's, typically — while `send()` is
     * blocked waiting. It only writes members that the waiting side re-reads through
     * `volatile`, and never allocates or calls back into the SDK.
     */
    void on_host_frame(const uint8_t *data, size_t len);

    /** True once the link is up and the host has announced a protocol version we speak. */
    bool host_ready() const
    {
        return attached_.load(std::memory_order_relaxed)
            && hello_seen_.load(std::memory_order_acquire);
    }

    /**
     * Chunk the request to the host and wait for its verdict.
     *
     * Refuses any URL whose host is not the DSN's, so a bug on the device cannot turn the
     * companion app into an open proxy. The app must enforce the same rule independently.
     */
    Response send(
        const char *url, const Headers &headers, const uint8_t *body, size_t len) override;
    bool is_available() override { return host_ready(); }
    const char *name() const override { return "relay"; }

private:
    bool write_frame(const uint8_t *frame, size_t len);
    Response await_status(uint8_t request_id);

    WriteFn write_;
    WaitFn wait_;
    ClockFn clock_;
    void *ctx_;

    size_t chunk_bytes_ = 180;
    /* What we were configured with, kept separately so a HELLO from a host with a small MTU
     * can clamp the chunk size without permanently overwriting the caller's choice for the
     * next host that connects. */
    size_t configured_chunk_bytes_ = 180;
    uint32_t timeout_ms_ = 20000;

    uint8_t next_request_id_ = 1;

    std::atomic<bool> attached_ { false };
    std::atomic<bool> hello_seen_ { false };

    /*
     * Written by whatever task delivers the host's frames — on ESP32 the BLE stack's, which
     * can be on the *other core* — and read by the task blocked in send().
     *
     * `status_pending_` is the handshake, and it has to be an acquire/release pair rather
     * than merely `volatile`. Volatile orders volatile accesses against each other and
     * nothing else: neither the compiler nor a weakly-ordered core is stopped from moving
     * the plain writes to `status_response_` after the flag that publishes them. The reader
     * would then see `pending == true` and read a half-written response. Rare, cross-core,
     * and impossible to reproduce on demand — exactly the bug worth spending two atomics on.
     *
     * Only one request is ever in flight, so a single flag is sufficient; no lock is needed.
     */
    std::atomic<bool> status_pending_ { false };
    uint8_t status_request_id_ = 0;
    sentry_response_t status_response_;
};

} // namespace sentry

#endif /* SENTRY_MICRO_TRANSPORT_RELAY_HPP_INCLUDED */
