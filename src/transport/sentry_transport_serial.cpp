#include "sentry_transport_serial.hpp"

#if defined(ARDUINO)

#    include <stdlib.h>
#    include <string.h>

#    include "../core/sentry_base64.h"
#    include "../sentry_micro.h"

namespace sentry {
namespace {

/**
 * Write one base64-encoded field, followed by a space.
 *
 * Encoded in fixed chunks straight to the stream rather than into a buffer: a 4 KB
 * envelope would need a 5.4 KB base64 buffer, and the Arduino loop task only has 8 KB of
 * stack in total. This way the cost is 65 bytes regardless of envelope size.
 *
 * The chunk is a multiple of 3 so that base64 padding can only ever occur on the final,
 * short chunk — padding mid-stream would corrupt the encoding.
 */
void write_field(Stream *stream, const uint8_t *data, size_t len)
{
    const size_t chunk_bytes = 48;
    char encoded[SENTRY_BASE64_ENCODED_LEN(48) + 1];

    for (size_t offset = 0; offset < len; offset += chunk_bytes) {
        size_t take = len - offset;
        if (take > chunk_bytes) {
            take = chunk_bytes;
        }
        sentry_base64_encode(encoded, sizeof(encoded), data + offset, take);
        stream->print(encoded);
    }
    stream->print(' ');
}

void write_field(Stream *stream, const char *text)
{
    write_field(stream, (const uint8_t *)text, strlen(text));
}

/**
 * Read one newline-terminated line into `buf`, discarding anything that arrives before the
 * deadline but is not a complete line.
 *
 * Returns false on timeout. Note this deliberately keeps reading past non-matching lines:
 * the host script echoes device output back to a terminal and a user may be typing, so the
 * result line is not guaranteed to be the very next thing received.
 */
bool read_line(Stream *stream, char *buf, size_t cap, uint32_t deadline_ms)
{
    size_t len = 0;
    while ((int32_t)(deadline_ms - millis()) > 0) {
        if (!stream->available()) {
            /* Yield rather than spin: on a single-core part this task starving the idle
             * task for the whole timeout is how a watchdog reboot happens. */
            delay(2);
            continue;
        }
        int c = stream->read();
        if (c < 0) {
            continue;
        }
        if (c == '\n') {
            buf[len] = '\0';
            return true;
        }
        if (c == '\r') {
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = (char)c;
        }
        /* An over-long line is truncated rather than abandoned; the prefix check below
         * will reject it if it was not ours. */
    }
    return false;
}

} // namespace

Response SerialTransport::send(
    const char *url, const Headers &headers, const uint8_t *body, size_t len)
{
    if (!stream_ || !url || !body || len == 0) {
        return SEND_REJECTED;
    }
    if (len > MAX_ENVELOPE_BYTES) {
        /* Refuse rather than truncate: half an envelope is not a smaller event, it is a
         * malformed one that ingest will reject after the host has done the work. */
        return SEND_REJECTED;
    }

    /* The host performs the request, so it — not the device — is what a rogue URL would
     * abuse. The script enforces its own whitelist, and this is the matching check on the
     * near side so neither end has to trust the other. */
    const sentry_dsn_t *dsn = sentry_get_dsn();
    if (!dsn || !dsn->valid || !sentry_url_host_matches(url, dsn->host)) {
        return SEND_REJECTED;
    }

    stream_->print(SENTRY_RELAY_REQUEST_PREFIX);
    write_field(stream_, url);
    write_field(stream_, headers.auth ? headers.auth : "");
    write_field(
        stream_, headers.content_type ? headers.content_type : "application/x-sentry-envelope");
    write_field(stream_, body, len);
    stream_->print('\n');
    stream_->flush();

    /* Wait for the host's verdict. A missing relay simply never answers, which times out
     * into "no route" — exactly what the caller should buffer and retry against. */
    uint32_t deadline = millis() + timeout_ms_;
    char line[96];
    const size_t prefix_len = strlen(SENTRY_RELAY_RESULT_PREFIX);
    while (read_line(stream_, line, sizeof(line), deadline)) {
        if (strncmp(line, SENTRY_RELAY_RESULT_PREFIX, prefix_len) != 0) {
            continue;
        }
        /* "<status> <retry_after_ms>" */
        char *cursor = line + prefix_len;
        long status = strtol(cursor, &cursor, 10);
        long retry_after_ms = strtol(cursor, NULL, 10);
        if (retry_after_ms < 0) {
            retry_after_ms = 0;
        }

        if (status <= 0) {
            /* The host reached nothing at all — its own network is down. */
            return Response(SEND_UNAVAILABLE);
        }
        if (status == 429) {
            return Response(SEND_RATE_LIMITED, (uint16_t)status, (uint32_t)retry_after_ms);
        }
        if (status >= 200 && status < 300) {
            return Response(SEND_OK, (uint16_t)status);
        }
        if (status >= 400 && status < 500) {
            return Response(SEND_REJECTED, (uint16_t)status, (uint32_t)retry_after_ms);
        }
        return Response(SEND_ERROR, (uint16_t)status, (uint32_t)retry_after_ms);
    }

    return Response(SEND_UNAVAILABLE);
}

} // namespace sentry

#endif /* ARDUINO */
