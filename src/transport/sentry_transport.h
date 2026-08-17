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

/**
 * What a transport reports back about one delivery attempt.
 *
 * A struct rather than a bare `SendResult` so that the interface can learn new facts about
 * a response — rate-limit categories, a server-supplied event id — without changing a
 * signature every transport implements. New fields get a default, so existing transports
 * keep compiling and simply leave them unset.
 *
 * Everything beyond `result` is optional. A transport that knows nothing but "it worked"
 * returns `SEND_OK` and is a complete, correct implementation; the conversion below makes
 * that a plain `return SEND_OK;`. The core treats unset fields as "no information" and
 * falls back to its own backoff policy rather than assuming zero means anything.
 */
struct Response {
    /** The outcome. The only field a transport must set. */
    SendResult result = SEND_ERROR;

    /** HTTP status if a server actually answered, else 0 (DNS/TLS/socket failure). */
    uint16_t http_status = 0;

    /**
     * How long to hold off before retrying, in milliseconds; 0 means "not stated".
     *
     * Comes from `Retry-After` or `X-Sentry-Rate-Limits` on a 429. Honouring it is not
     * politeness — a device in a boot loop that ignores it will hammer ingest with the
     * same crash forever, and get the project rate-limited for everyone.
     */
    uint32_t retry_after_ms = 0;

    Response() = default;

    /* Implicit on purpose: it is what lets a minimal transport `return SEND_OK;`. */
    Response(SendResult result_, uint16_t http_status_ = 0, uint32_t retry_after_ms_ = 0)
        : result(result_)
        , http_status(http_status_)
        , retry_after_ms(retry_after_ms_)
    {
    }
};

class Transport {
public:
    virtual ~Transport() = default;

    /**
     * Deliver `len` bytes of envelope `body` to `url`.
     *
     * May block: the core calls this from a context where blocking is acceptable, never
     * from `loop()` behind the caller's back. It must not allocate on the failure path —
     * this can run after a crash, where the heap is not trustworthy.
     *
     * A minimal implementation is a POST and a status check:
     *
     *     Response send(const char *url, const Headers &h,
     *                   const uint8_t *body, size_t len) override {
     *         if (!post(url, h, body, len)) { return SEND_UNAVAILABLE; }
     *         return SEND_OK;
     *     }
     */
    virtual Response send(const char *url, const Headers &headers, const uint8_t *body, size_t len)
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
