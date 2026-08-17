#include "sentry_transport_wifi.hpp"

#if defined(ARDUINO)

#    include <HTTPClient.h>
#    include <stdlib.h>
#    include <string.h>
#    include <WiFi.h>
#    include <WiFiClientSecure.h>

#    include "../sentry_micro.h"

namespace sentry {
namespace {

void log_line(const char *message)
{
    sentry_logger_fn logger = sentry_get_logger();
    if (logger) {
        logger(message);
    }
}

/**
 * Parse a `Retry-After` value into milliseconds.
 *
 * Sentry sends delta-seconds. The HTTP-date form is legal but would need a parsed clock
 * the device may not have, so an unparseable value yields 0 and the core falls back to
 * its own backoff — better than treating "I could not read this" as "retry immediately".
 */
uint32_t parse_retry_after_ms(const String &value)
{
    if (value.length() == 0) {
        return 0;
    }
    char *end = nullptr;
    long seconds = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || seconds <= 0) {
        return 0;
    }
    /* Clamp: a hostile or garbled header should not park the device for a month. */
    const long max_seconds = 24L * 60 * 60;
    if (seconds > max_seconds) {
        seconds = max_seconds;
    }
    return (uint32_t)seconds * 1000u;
}

/**
 * `X-Sentry-Rate-Limits: <retry_after>:<categories>:<scope>:...`, comma-separated.
 * Only the leading delay is used for now; per-category limits need the core to track
 * which categories it is holding, which is a later milestone.
 */
uint32_t parse_sentry_rate_limits_ms(const String &value)
{
    if (value.length() == 0) {
        return 0;
    }
    return parse_retry_after_ms(value);
}

} // namespace

static Response post_envelope(WiFiClient &client, const char *url, const Headers &headers,
    const uint8_t *body, size_t len, uint32_t timeout_ms);

bool WiFiTransport::is_available()
{
    /* WL_CONNECTED alone can be true for a moment before DHCP completes, and a POST then
     * fails on a bind rather than reporting "no route". Requiring an address makes the
     * answer mean what the auto-select chain assumes it means. */
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != INADDR_NONE;
}

Response WiFiTransport::send(
    const char *url, const Headers &headers, const uint8_t *body, size_t len)
{
    if (!url || !body || len == 0) {
        return SEND_REJECTED;
    }
    if (!is_available()) {
        /* Not an error: the caller should buffer this and try again later. */
        return SEND_UNAVAILABLE;
    }

    /* Whitelist before anything touches the network. Defaults to the DSN's host, so this
     * holds without the user configuring anything. */
    const sentry_dsn_t *dsn = sentry_get_dsn();
    const char *allowed = allowed_host_;
    if (!allowed && dsn && dsn->valid) {
        allowed = dsn->host;
    }
    if (!allowed || !sentry_url_host_matches(url, allowed)) {
        log_line("wifi transport refused a URL outside the configured ingest host");
        return SEND_REJECTED;
    }

    const bool is_https = strncmp(url, "https://", 8) == 0;

    /* Build only the client actually needed: WiFiClientSecure allocates an mbedTLS context
     * on construction, which is wasted on a plain-HTTP self-hosted endpoint. */
    if (is_https) {
        WiFiClientSecure client;
        if (ca_cert_) {
            client.setCACert(ca_cert_);
        } else {
            if (!warned_insecure_) {
                warned_insecure_ = true;
                log_line("TLS certificate verification is OFF; call set_ca_cert() to enable it");
            }
            client.setInsecure();
        }
        client.setTimeout(timeout_ms_ / 1000);
        return post_envelope(client, url, headers, body, len, timeout_ms_);
    }

    WiFiClient client;
    client.setTimeout(timeout_ms_ / 1000);
    return post_envelope(client, url, headers, body, len, timeout_ms_);
}

/**
 * Issue the POST over an already-configured client.
 *
 * A free function, not a member, so the header never names Arduino's client type — see the
 * note in the class. `WiFiClient` is spelled here where <WiFi.h> has already defined it,
 * which works on both Arduino core 2.x (where it is a class) and 3.x (a typedef for
 * NetworkClient).
 */
static Response post_envelope(WiFiClient &client, const char *url, const Headers &headers,
    const uint8_t *body, size_t len, uint32_t timeout_ms)
{
    HTTPClient http;
    if (!http.begin(client, url)) {
        return SEND_ERROR;
    }

    http.setConnectTimeout((int32_t)timeout_ms);
    http.setTimeout((uint16_t)timeout_ms);
    /* Ingest never redirects, and following one would send the auth header to wherever the
     * redirect pointed — the same open-proxy hazard the host whitelist exists to prevent. */
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent(SENTRY_MICRO_SDK_USER_AGENT);
    http.addHeader("Content-Type",
        headers.content_type ? headers.content_type : "application/x-sentry-envelope");
    http.addHeader("X-Sentry-Auth", headers.auth ? headers.auth : "");

    /* Not `const char *const[]`: HTTPClient::collectHeaders takes `const char **`. */
    static const char *collected[] = { "Retry-After", "X-Sentry-Rate-Limits" };
    http.collectHeaders(collected, sizeof(collected) / sizeof(collected[0]));

    /* HTTPClient's POST takes a non-const pointer but does not modify the buffer. */
    int status = http.POST(const_cast<uint8_t *>(body), len);

    Response response;
    if (status < 0) {
        /* Negative values are HTTPClient's own errors, not HTTP statuses. A refused or
         * failed connection is "no route right now" — worth retrying — while a garbled
         * response or a timeout is a genuine error. */
        response
            = (status == HTTPC_ERROR_CONNECTION_REFUSED || status == HTTPC_ERROR_CONNECTION_LOST
                  || status == HTTPC_ERROR_NO_HTTP_SERVER)
            ? Response(SEND_UNAVAILABLE)
            : Response(SEND_ERROR);
    } else {
        uint32_t retry_after_ms = parse_retry_after_ms(http.header("Retry-After"));
        if (retry_after_ms == 0) {
            retry_after_ms = parse_sentry_rate_limits_ms(http.header("X-Sentry-Rate-Limits"));
        }

        if (status == 429) {
            response = Response(SEND_RATE_LIMITED, (uint16_t)status, retry_after_ms);
        } else if (status >= 200 && status < 300) {
            response = Response(SEND_OK, (uint16_t)status);
        } else if (status >= 400 && status < 500) {
            /* A 4xx means ingest understood the request and will not accept it — a bad
             * key, or a malformed envelope. Retrying cannot change that, and a device in a
             * boot loop retrying forever is how a project gets rate-limited for everyone. */
            response = Response(SEND_REJECTED, (uint16_t)status, retry_after_ms);
        } else {
            response = Response(SEND_ERROR, (uint16_t)status, retry_after_ms);
        }
    }

    http.end();
    return response;
}

} // namespace sentry

#endif /* ARDUINO */
