/**
 * The transport interface.
 *
 * This is the pivot the whole design turns on. The library owns *all* Sentry semantics —
 * DSN parsing, envelope construction, the auth header, rate limiting — and delivery is
 * reduced to a single call that moves opaque bytes to a URL. That is what lets the same
 * core reach Sentry over WiFi, BLE, serial, or LoRa, and it is why a companion app needs
 * zero Sentry knowledge to relay for a device with no internet of its own.
 *
 * Plain C, and in `core/` rather than `transport/`, because the interface itself is
 * portable: no Arduino, no ESP-IDF, no allocation. That keeps a transport writable from an
 * ESP-IDF component with no C++ runtime at all. C++ users get an ergonomic base class in
 * `transport/sentry_transport.hpp` that wraps this — the vtable below is what actually
 * crosses the boundary either way.
 */
#ifndef SENTRY_MICRO_TRANSPORT_H_INCLUDED
#define SENTRY_MICRO_TRANSPORT_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The headers an envelope POST requires. An explicit struct rather than a map: ingest
 * needs exactly these two, and a map would mean allocation on the crash path.
 */
typedef struct {
    /** Value for `X-Sentry-Auth`, produced by `sentry_dsn_auth_header`. */
    const char *auth;
    /** Always `application/x-sentry-envelope` today; explicit so transports don't guess. */
    const char *content_type;
} sentry_headers_t;

/** Result of a delivery attempt. Negative values are transport-level failures. */
typedef enum {
    /** Delivered; `2xx` from ingest. */
    SENTRY_SEND_OK = 0,
    /** No route right now (WiFi down, no relay attached). Caller should buffer and retry. */
    SENTRY_SEND_UNAVAILABLE = -1,
    /** The request was made and failed in a way that retrying will not fix (`4xx`). */
    SENTRY_SEND_REJECTED = -2,
    /** Rate limited (`429`) — back off, do not spin. */
    SENTRY_SEND_RATE_LIMITED = -3,
    /** Anything else: DNS, TLS, socket, timeout. */
    SENTRY_SEND_ERROR = -4,
} sentry_send_result_t;

/**
 * What a transport reports back about one delivery attempt.
 *
 * A struct rather than a bare result code so the interface can learn new facts about a
 * response — rate-limit categories, a server-supplied event id — without changing a
 * signature every transport implements. New fields are added at the end and zero-valued
 * for transports that do not know about them.
 *
 * Everything beyond `result` is optional. A transport that knows nothing but "it worked"
 * is a complete, correct implementation. The core treats a zero field as "no information"
 * and falls back to its own policy rather than assuming zero means anything.
 */
typedef struct {
    /** The outcome. The only field a transport must set. */
    sentry_send_result_t result;
    /** HTTP status if a server actually answered, else 0 (DNS/TLS/socket failure). */
    uint16_t http_status;
    /**
     * How long to hold off before retrying, in milliseconds; 0 means "not stated".
     *
     * Comes from `Retry-After` or `X-Sentry-Rate-Limits` on a 429. Honouring it is not
     * politeness — a device in a boot loop that ignores it will hammer ingest with the
     * same crash forever, and get the project rate-limited for everyone.
     */
    uint32_t retry_after_ms;
} sentry_response_t;

/** Convenience for the common cases, so a C transport need not fill the struct by hand. */
static inline sentry_response_t sentry_response_make(sentry_send_result_t result)
{
    sentry_response_t response;
    response.result = result;
    response.http_status = 0;
    response.retry_after_ms = 0;
    return response;
}

/**
 * A transport: three function pointers and an opaque context.
 *
 * Only `send` is required. `is_available` and `name` may be NULL, and the accessors below
 * supply sane behaviour when they are — so the minimum viable transport is one function
 * and a designated initialiser.
 */
typedef struct sentry_transport_s {
    /**
     * Deliver `len` bytes of envelope `body` to `url`. Required.
     *
     * May block: the core calls this from a context where blocking is acceptable, never
     * from the user's main loop behind their back. It must not allocate on the failure
     * path — this can run after a crash, where the heap is not trustworthy.
     */
    sentry_response_t (*send)(void *ctx, const char *url, const sentry_headers_t *headers,
        const uint8_t *body, size_t len);

    /**
     * Cheap, non-blocking "is there any point calling send() right now?". Optional.
     * Lets the auto-select chain skip a dead transport without paying a connect timeout.
     */
    bool (*is_available)(void *ctx);

    /** Short name for debug logs, e.g. "wifi". Optional. */
    const char *(*name)(void *ctx);

    /** Passed back to every callback. The C++ wrapper stores the object pointer here. */
    void *ctx;
} sentry_transport_t;

/** Call `transport->send`, or report `SENTRY_SEND_UNAVAILABLE` if there is nothing to call. */
sentry_response_t sentry_transport_send(const sentry_transport_t *transport, const char *url,
    const sentry_headers_t *headers, const uint8_t *body, size_t len);

/** Availability, defaulting to true when the transport does not implement the check. */
bool sentry_transport_is_available(const sentry_transport_t *transport);

/** Transport name, defaulting to "unknown". Never returns NULL. */
const char *sentry_transport_name(const sentry_transport_t *transport);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_TRANSPORT_H_INCLUDED */
