#include "sentry_transport.h"

sentry_response_t sentry_transport_send(const sentry_transport_t *transport, const char *url,
    const sentry_headers_t *headers, const uint8_t *body, size_t len)
{
    /* A NULL transport is not a programming error to assert on — it is the ordinary state
     * of a device that has not been given one yet, or whose transport was torn down. It
     * means "no route", which is exactly what the caller should buffer and retry against. */
    if (!transport || !transport->send) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }
    return transport->send(transport->ctx, url, headers, body, len);
}

bool sentry_transport_is_available(const sentry_transport_t *transport)
{
    if (!transport || !transport->send) {
        return false;
    }
    /* Not implementing the check means "always worth trying", not "never". */
    if (!transport->is_available) {
        return true;
    }
    return transport->is_available(transport->ctx);
}

const char *sentry_transport_name(const sentry_transport_t *transport)
{
    if (!transport || !transport->name) {
        return "unknown";
    }
    const char *name = transport->name(transport->ctx);
    return name ? name : "unknown";
}
