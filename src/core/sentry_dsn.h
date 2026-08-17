/**
 * DSN parsing and ingest-URL construction.
 *
 * A Sentry DSN looks like:
 *
 *     https://<public_key>@<host>[:<port>][/<path>]/<project_id>
 *
 * From it we derive the two things the device actually needs to talk to Sentry:
 *
 *     POST <scheme>://<host>[:<port>][/<path>]/api/<project_id>/envelope/
 *     X-Sentry-Auth: Sentry sentry_version=7, sentry_client=..., sentry_key=<public_key>
 *
 * That is the whole ingest contract. It is SDK-agnostic — which is exactly what makes
 * "the device builds the envelope, the transport is a dumb pipe" viable on a chip with
 * no room for a real SDK.
 *
 * Portable C99: no Arduino, no ESP-IDF, no allocation. Host-testable.
 */
#ifndef SENTRY_MICRO_DSN_H_INCLUDED
#define SENTRY_MICRO_DSN_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** True only if parsing succeeded; every other field is meaningless otherwise. */
    bool valid;
    /** True for `https://`, false for `http://`. Selects the port and the TLS path. */
    bool is_secure;
    /** Public key (the part before `@`). Goes into the auth header, never the URL. */
    char public_key[SENTRY_MICRO_MAX_KEY_LEN];
    /** Bare hostname, no scheme, no port. Transports whitelist against this. */
    char host[SENTRY_MICRO_MAX_HOST_LEN];
    /** Optional path prefix between host and project id, "" for sentry.io. Leading '/'. */
    char path[SENTRY_MICRO_MAX_PATH_LEN];
    /** Numeric project id (kept as a string; it is only ever concatenated). */
    char project_id[SENTRY_MICRO_MAX_PROJECT_ID_LEN];
    /**
     * Organization id, recovered from an `o<digits>.` host prefix — `o1234.ingest.…`
     * yields `"1234"`. Empty when the host does not carry one (self-hosted, or a custom
     * ingest domain), which is not an error.
     *
     * This does *not* affect where events are sent. It rides along in the trace header
     * and the Dynamic Sampling Context, where it lets Sentry tell "this incoming trace
     * belongs to my org" from "this one is someone else's".
     */
    char org_id[SENTRY_MICRO_MAX_ORG_ID_LEN];
    /** Explicit port, or 0 when the DSN did not specify one (use the scheme default). */
    uint16_t port;
} sentry_dsn_t;

/**
 * Parse `dsn` into `out`.
 *
 * Returns true on success. On failure `out->valid` is false and the SDK stays disabled
 * rather than half-configured — a device that silently POSTs to a mis-parsed host is
 * worse than one that reports nothing.
 */
bool sentry_dsn_parse(sentry_dsn_t *out, const char *dsn);

/**
 * Write the envelope ingest endpoint into `buf`.
 *
 * The port is emitted only when the DSN specified one explicitly, so the common
 * `https://…@o0.ingest.sentry.io/123` DSN yields a clean URL with no `:443`.
 *
 * Returns the number of bytes written (excluding the NUL), or 0 if the DSN is invalid
 * or `buf` is too small.
 */
size_t sentry_dsn_envelope_url(const sentry_dsn_t *dsn, char *buf, size_t buf_len);

/**
 * Write the `X-Sentry-Auth` header *value* into `buf` (the header name is not included).
 *
 * `sentry_version=7` is the current ingest protocol version. We deliberately omit
 * `sentry_timestamp`: it is optional, ignored by modern ingest, and an MCU frequently
 * has no wall clock before NTP has run.
 *
 * Returns the number of bytes written (excluding the NUL), or 0 on failure.
 */
size_t sentry_dsn_auth_header(const sentry_dsn_t *dsn, char *buf, size_t buf_len);

/**
 * Resolve the effective org id, given an optional explicit override.
 *
 * `override` wins when it is non-NULL and non-empty; otherwise the value recovered from
 * the DSN host is used. This matters for self-hosted and custom-domain setups, where the
 * host carries no `o<digits>.` prefix and the org id can only be configured by hand.
 *
 * Never returns NULL — the result is `""` when nothing is known.
 */
const char *sentry_dsn_resolve_org_id(const sentry_dsn_t *dsn, const char *override);

/**
 * True when the host component of `url` is exactly `host`, compared case-insensitively.
 *
 * This is a security check, not a convenience. Every transport must refuse to POST
 * anywhere but the configured ingest host: the relay transport asks a *phone* to make the
 * request on the device's behalf, so without this a buggy or hostile device turns its
 * companion app into an open proxy. The WiFi transport applies the same rule so the
 * guarantee does not depend on which transport happens to be selected.
 *
 * Matching is exact — no suffix matching. `evil-sentry.io` must not pass for `sentry.io`,
 * and neither must `sentry.io.evil.com`.
 */
bool sentry_url_host_matches(const char *url, const char *host);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_DSN_H_INCLUDED */
