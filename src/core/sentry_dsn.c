#include "sentry_dsn.h"

#include <stdio.h>
#include <string.h>

/**
 * Copy `len` bytes of `src` into a fixed `dst[cap]`, NUL-terminating.
 * Returns false if it would not fit — the caller turns that into a parse failure
 * rather than silently truncating a hostname (which would send events elsewhere).
 */
static bool copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
    if (len + 1 > cap) {
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static bool parse_port(const char *src, size_t len, uint16_t *out)
{
    uint32_t port = 0;
    if (len == 0 || len > 5) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (src[i] < '0' || src[i] > '9') {
            return false;
        }
        port = port * 10 + (uint32_t)(src[i] - '0');
    }
    if (port == 0 || port > 65535) {
        return false;
    }
    *out = (uint16_t)port;
    return true;
}

bool sentry_dsn_parse(sentry_dsn_t *out, const char *dsn)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!dsn) {
        return false;
    }

    size_t dsn_len = strlen(dsn);
    if (dsn_len == 0 || dsn_len >= SENTRY_MICRO_MAX_DSN_LEN) {
        return false;
    }

    /* ── scheme ── */
    const char *cursor;
    if (strncmp(dsn, "https://", 8) == 0) {
        out->is_secure = true;
        cursor = dsn + 8;
    } else if (strncmp(dsn, "http://", 7) == 0) {
        out->is_secure = false;
        cursor = dsn + 7;
    } else {
        return false;
    }

    /* ── public key ── */
    const char *at = strchr(cursor, '@');
    if (!at || at == cursor) {
        return false;
    }
    /* Legacy DSNs carried `public:secret@`. The secret has been unused by ingest for
     * years; accept those DSNs but keep only the public half. */
    size_t key_len = (size_t)(at - cursor);
    const char *colon = memchr(cursor, ':', key_len);
    if (colon) {
        key_len = (size_t)(colon - cursor);
    }
    if (key_len == 0 || !copy_bounded(out->public_key, sizeof(out->public_key), cursor, key_len)) {
        return false;
    }
    cursor = at + 1;

    /* ── host[:port] ── */
    const char *slash = strchr(cursor, '/');
    if (!slash) {
        /* No path at all means no project id. */
        return false;
    }
    size_t authority_len = (size_t)(slash - cursor);
    size_t host_len = authority_len;
    const char *port_colon = memchr(cursor, ':', authority_len);
    if (port_colon) {
        host_len = (size_t)(port_colon - cursor);
        if (!parse_port(port_colon + 1, authority_len - host_len - 1, &out->port)) {
            return false;
        }
    }
    if (host_len == 0 || !copy_bounded(out->host, sizeof(out->host), cursor, host_len)) {
        return false;
    }

    /* ── [/path]/project_id ──
     * `slash` points at the '/' that starts the path. Trailing slashes are tolerated
     * (people copy DSNs out of the UI with one) and stripped before we split. */
    const char *path_start = slash;
    const char *path_end = dsn + dsn_len;
    while (path_end > path_start && path_end[-1] == '/') {
        path_end--;
    }

    /* The project id is the last segment; anything before it is a path prefix, which
     * self-hosted Sentry behind a reverse proxy (`https://…@example.com/sentry/42`)
     * needs preserved in the ingest URL. */
    const char *last_slash = path_end;
    while (last_slash > path_start && last_slash[-1] != '/') {
        last_slash--;
    }
    size_t project_len = (size_t)(path_end - last_slash);
    if (project_len == 0
        || !copy_bounded(out->project_id, sizeof(out->project_id), last_slash, project_len)) {
        return false;
    }
    /* Project ids are numeric; rejecting non-digits catches a DSN that lost its id. */
    for (size_t i = 0; i < project_len; i++) {
        if (out->project_id[i] < '0' || out->project_id[i] > '9') {
            return false;
        }
    }

    size_t prefix_len = (size_t)(last_slash - path_start);
    if (prefix_len > 0) {
        /* Drop the separating '/' that last_slash sits after, keep the leading one. */
        if (!copy_bounded(out->path, sizeof(out->path), path_start, prefix_len - 1)) {
            return false;
        }
    }

    out->valid = true;
    return true;
}

size_t sentry_dsn_envelope_url(const sentry_dsn_t *dsn, char *buf, size_t buf_len)
{
    if (!dsn || !dsn->valid || !buf || buf_len == 0) {
        return 0;
    }

    char port_part[8] = { 0 };
    if (dsn->port != 0) {
        snprintf(port_part, sizeof(port_part), ":%u", (unsigned)dsn->port);
    }

    int written = snprintf(buf, buf_len, "%s://%s%s%s/api/%s/envelope/",
        dsn->is_secure ? "https" : "http", dsn->host, port_part, dsn->path, dsn->project_id);
    if (written < 0 || (size_t)written >= buf_len) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

size_t sentry_dsn_auth_header(const sentry_dsn_t *dsn, char *buf, size_t buf_len)
{
    if (!dsn || !dsn->valid || !buf || buf_len == 0) {
        return 0;
    }
    int written = snprintf(buf, buf_len, "Sentry sentry_version=7, sentry_client=%s, sentry_key=%s",
        SENTRY_MICRO_SDK_USER_AGENT, dsn->public_key);
    if (written < 0 || (size_t)written >= buf_len) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)written;
}
