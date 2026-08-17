/**
 * WiFi transport — HTTPS POST straight to Sentry ingest.
 *
 * The self-contained case: a device on WiFi needs nothing else to report. Everything
 * Sentry-specific has already happened by the time this runs; it receives a URL, two
 * headers and a byte buffer, and its only jobs are to make the request and translate the
 * answer back into a `Response` the core can act on.
 *
 *     static sentry::WiFiTransport transport;   // file scope: must outlive the SDK
 *     sentry::set_transport(transport);
 *
 * **Do not call this from a panic handler.** mbedTLS allocates several kilobytes during a
 * handshake, and the heap is exactly what you cannot trust immediately after a crash. The
 * design does not need it to: a crash is detected on the *next* boot from the reset reason
 * and the coredump partition, and reported from normal runtime where allocating is fine.
 * That is why the no-allocation rule is stated for the core and not for transports.
 */
#ifndef SENTRY_MICRO_TRANSPORT_WIFI_HPP_INCLUDED
#define SENTRY_MICRO_TRANSPORT_WIFI_HPP_INCLUDED

#include "sentry_transport.hpp"

#if defined(ARDUINO)

class WiFiClient;

namespace sentry {

class WiFiTransport : public Transport {
public:
    WiFiTransport() = default;

    /**
     * POST `body` to `url`.
     *
     * Refuses any URL whose host is not the DSN's ingest host, so a bug elsewhere cannot
     * turn the device into a beacon for somebody else's server.
     */
    Response send(
        const char *url, const Headers &headers, const uint8_t *body, size_t len) override;

    /** True only while the station interface has an association and an address. */
    bool is_available() override;

    const char *name() const override { return "wifi"; }

    /**
     * Verify the server against this PEM root certificate.
     *
     * Strongly recommended, and stored by pointer — pass something with static lifetime.
     * Without it the connection is encrypted but *unauthenticated*: it will not be read by
     * a passive observer, but nothing stops a captive portal or a MITM from impersonating
     * ingest. The SDK logs a warning the first time it sends without one.
     *
     * Left unset by default because pinning a root that expires bricks reporting on every
     * deployed device at once, and a maker with no CI has no way to push a new one. That
     * trade belongs to whoever ships the firmware, so it is a deliberate choice here rather
     * than a default either way.
     */
    void set_ca_cert(const char *pem_root_certificate) { ca_cert_ = pem_root_certificate; }

    /** Connect and response timeout. Default 10s. */
    void set_timeout_ms(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }

    /**
     * Restrict POSTs to this host instead of the DSN's.
     * Only needed when routing through a proxy you control; the default is safer.
     */
    void set_allowed_host(const char *host) { allowed_host_ = host; }

private:
    Response post(WiFiClient &client, const char *url, const Headers &headers, const uint8_t *body,
        size_t len);

    const char *ca_cert_ = nullptr;
    const char *allowed_host_ = nullptr;
    uint32_t timeout_ms_ = 10000;
    bool warned_insecure_ = false;
};

} // namespace sentry

#endif /* ARDUINO */

#endif /* SENTRY_MICRO_TRANSPORT_WIFI_HPP_INCLUDED */
