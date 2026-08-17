/**
 * C++ ergonomics over the C transport interface.
 *
 * The vtable in `core/sentry_transport.h` is what the SDK actually calls, and a C user can
 * fill it in directly. This header exists so a C++ user does not have to: subclass
 * `sentry::Transport`, override one method, and hand the object to the SDK. The base class
 * keeps a `sentry_transport_t` whose function pointers are static trampolines back to the
 * virtuals, so there is exactly one representation crossing the boundary.
 *
 * Cost of the wrapper: one vtable pointer and one 4-pointer struct per instance, and there
 * are typically one or two instances in a firmware.
 */
#ifndef SENTRY_MICRO_TRANSPORT_HPP_INCLUDED
#define SENTRY_MICRO_TRANSPORT_HPP_INCLUDED

#include "../core/sentry_transport.h"

namespace sentry {

/* The C types are the types. Aliases rather than parallel definitions, so nothing has to
 * be converted at the boundary and the two APIs cannot drift apart. */
using Headers = sentry_headers_t;
using SendResult = sentry_send_result_t;

/* Shorter spellings of the enum for C++ callers; same values. */
const SendResult SEND_OK = SENTRY_SEND_OK;
const SendResult SEND_UNAVAILABLE = SENTRY_SEND_UNAVAILABLE;
const SendResult SEND_REJECTED = SENTRY_SEND_REJECTED;
const SendResult SEND_RATE_LIMITED = SENTRY_SEND_RATE_LIMITED;
const SendResult SEND_ERROR = SENTRY_SEND_ERROR;

/**
 * `sentry_response_t` plus constructors.
 *
 * Derives from the C struct rather than wrapping it: it adds no members, so it stays
 * standard-layout and converts to the C type for free. The implicit constructor from a
 * `SendResult` is the whole point — it is what lets a transport that knows nothing but
 * "it worked" simply `return SEND_OK;`.
 */
struct Response : sentry_response_t {
    Response() { *static_cast<sentry_response_t *>(this) = sentry_response_make(SEND_ERROR); }

    Response(SendResult result_, uint16_t http_status_ = 0, uint32_t retry_after_ms_ = 0)
    {
        result = result_;
        http_status = http_status_;
        retry_after_ms = retry_after_ms_;
    }

    Response(const sentry_response_t &other) { *static_cast<sentry_response_t *>(this) = other; }
};

/**
 * Base class for a C++ transport.
 *
 * A complete implementation overrides `send()` and `name()`:
 *
 *     class MyTransport : public sentry::Transport {
 *     public:
 *         sentry::Response send(const char *url, const sentry::Headers &h,
 *                               const uint8_t *body, size_t len) override {
 *             return my_post(url, h.auth, body, len) ? sentry::SEND_OK
 *                                                    : sentry::SEND_UNAVAILABLE;
 *         }
 *         const char *name() const override { return "mine"; }
 *     };
 */
class Transport {
public:
    Transport()
    {
        c_transport_.send = &trampoline_send;
        c_transport_.is_available = &trampoline_is_available;
        c_transport_.name = &trampoline_name;
        c_transport_.ctx = this;
    }

    virtual ~Transport() = default;

    /* Non-copyable: `ctx` points at `this`, so a copy would carry a pointer to the
     * original and quietly dispatch to the wrong object. */
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    /** Deliver `len` bytes of envelope `body` to `url`. See the C header for the contract. */
    virtual Response send(const char *url, const Headers &headers, const uint8_t *body, size_t len)
        = 0;

    /** Cheap, non-blocking "is there any point calling send() right now?". */
    virtual bool is_available() { return true; }

    /** Short name for debug logs, e.g. "wifi". */
    virtual const char *name() const = 0;

    /**
     * The C view of this object — what gets handed to the SDK.
     * Valid for as long as the object is; do not register a stack-local transport.
     */
    const sentry_transport_t *c_transport() const { return &c_transport_; }

private:
    static sentry_response_t trampoline_send(void *ctx, const char *url,
        const sentry_headers_t *headers, const uint8_t *body, size_t len)
    {
        /* The core never passes NULL headers, but a hand-written C caller might, and a
         * null-dereference inside a crash reporter is a particularly bad failure. */
        static const sentry_headers_t empty = { "", "" };
        return static_cast<Transport *>(ctx)->send(url, headers ? *headers : empty, body, len);
    }

    static bool trampoline_is_available(void *ctx)
    {
        return static_cast<Transport *>(ctx)->is_available();
    }

    static const char *trampoline_name(void *ctx) { return static_cast<Transport *>(ctx)->name(); }

    sentry_transport_t c_transport_;
};

} // namespace sentry

#endif /* SENTRY_MICRO_TRANSPORT_HPP_INCLUDED */
