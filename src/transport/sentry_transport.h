/**
 * The transport interface.
 *
 * This is the pivot the whole design turns on. The library owns *all* Sentry semantics —
 * DSN parsing, envelope construction, the auth header, rate limiting — and delivery is
 * reduced to a single method that moves opaque bytes to a URL. That is what lets the
 * same core reach Sentry over WiFi, BLE, serial, or LoRa, and it is why a companion app
 * needs zero Sentry knowledge to relay for a device that has no internet of its own.
 *
 * Implementations live in `src/transport/`; users can subclass this for anything else.
 */
#ifndef SENTRY_MICRO_TRANSPORT_H_INCLUDED
#define SENTRY_MICRO_TRANSPORT_H_INCLUDED

#include "../core/sentry_boot.h"

namespace sentry {

/**
 * The headers an envelope POST requires. Kept as an explicit struct rather than a map:
 * ingest needs exactly these two, and a map would mean allocation on the crash path.
 */
struct Headers {
    /** Value for `X-Sentry-Auth`, produced by `sentry_dsn_auth_header`. */
    const char *auth;
    /** Always `application/x-sentry-envelope` today; explicit so transports don't guess. */
    const char *content_type;
};

/** Result of a delivery attempt. Negative values are transport-level failures. */
enum SendResult {
    /** Delivered; `2xx` from ingest. */
    SEND_OK = 0,
    /** No route right now (WiFi down, no relay attached). Caller should buffer and retry. */
    SEND_UNAVAILABLE = -1,
    /** The request was made and failed in a way that retrying will not fix (`4xx`). */
    SEND_REJECTED = -2,
    /** Rate limited (`429`) — back off, do not spin. */
    SEND_RATE_LIMITED = -3,
    /** Anything else: DNS, TLS, socket, timeout. */
    SEND_ERROR = -4,
};

class Transport {
public:
    virtual ~Transport() = default;

    /**
     * Deliver `len` bytes of envelope `body` to `url`.
     *
     * Must not block indefinitely and must not allocate on the failure path — this can
     * be called from a post-crash context where the heap is not trustworthy.
     */
    virtual SendResult send(
        const char *url, const Headers &headers, const uint8_t *body, size_t len)
        = 0;

    /**
     * Cheap, non-blocking "is there any point calling send() right now?".
     * Used by the auto-select logic to skip a dead transport without paying for a
     * connection attempt.
     */
    virtual bool is_available() { return true; }

    /** Short name for debug logs and the `sdk.packages` metadata, e.g. "wifi". */
    virtual const char *name() const = 0;
};

} // namespace sentry

#endif /* SENTRY_MICRO_TRANSPORT_H_INCLUDED */
