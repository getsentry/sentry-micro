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

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_DSN_H_INCLUDED */
