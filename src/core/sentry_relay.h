/**
 * The relay wire protocol.
 *
 * A device with no internet of its own is usually connected to something that has one — a
 * phone over BLE, a laptop over USB, a gateway over LoRa. This is the framing that lets it
 * say "perform this HTTP request for me" over any link that can move short packets in both
 * directions, and get the answer back.
 *
 * It is deliberately **not** Sentry-specific. A frame carries a URL, two header values and a
 * body; a host that implements it needs no knowledge of envelopes, DSNs or ingest, and is
 * roughly twenty lines of application code. That is the entire premise of the relay design:
 * the device owns all the Sentry semantics, and the companion app is a dumb pipe.
 *
 * Portable C in `core/`, like the buffer and the envelope builder, because framing bugs are
 * silent and expensive to find on hardware — an off-by-one in an offset shows up as ingest
 * rejecting a truncated envelope hours later. Everything here is a pure function over
 * caller-supplied buffers, so the whole protocol is exercised by the host test suite.
 *
 * Frames, all integers little-endian:
 *
 *     device -> host
 *       0x01 BEGIN   [req_id u8][url_len u16][auth_len u16][ctype_len u16][body_len u32]
 *       0x02 DATA    [req_id u8][offset u16][payload...]
 *       0x03 END     [req_id u8]
 *     host -> device
 *       0x80 HELLO   [version u8][max_chunk u16]
 *       0x81 STATUS  [req_id u8][result u8][http_status u16][retry_after_ms u32]
 *
 * DATA payloads are slices of one logical byte stream — `url`, `auth`, `content_type` and
 * `body` concatenated in that order — and BEGIN gives the host the four lengths it needs to
 * cut it back apart. Concatenating rather than framing each field separately means a field
 * boundary never has to line up with a packet boundary, which is what keeps the encoder free
 * of special cases on a link whose MTU it does not control.
 */
#ifndef SENTRY_MICRO_RELAY_H_INCLUDED
#define SENTRY_MICRO_RELAY_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Protocol version, announced by the host in HELLO.
 *
 * The device refuses to relay through a host claiming a version it does not know, rather
 * than sending frames that would be misparsed. Bump this only for an incompatible change.
 */
#define SENTRY_RELAY_PROTOCOL_VERSION 1

#define SENTRY_RELAY_FRAME_BEGIN 0x01
#define SENTRY_RELAY_FRAME_DATA 0x02
#define SENTRY_RELAY_FRAME_END 0x03
#define SENTRY_RELAY_FRAME_HELLO 0x80
#define SENTRY_RELAY_FRAME_STATUS 0x81

#define SENTRY_RELAY_BEGIN_LEN 12
#define SENTRY_RELAY_DATA_HEADER_LEN 4
#define SENTRY_RELAY_END_LEN 2
#define SENTRY_RELAY_HELLO_LEN 4
#define SENTRY_RELAY_STATUS_LEN 9

/**
 * Smallest chunk a link may offer, in bytes.
 *
 * Below this the header dominates the payload and a 2 KB envelope needs hundreds of
 * packets; a BLE link with the default 23-byte MTU is already above it.
 */
#define SENTRY_RELAY_MIN_CHUNK_BYTES 24

/** Wire codes for the outcome, mapped to `sentry_send_result_t` on arrival. */
typedef enum {
    SENTRY_RELAY_RESULT_OK = 0,
    SENTRY_RELAY_RESULT_UNAVAILABLE = 1,
    SENTRY_RELAY_RESULT_REJECTED = 2,
    SENTRY_RELAY_RESULT_RATE_LIMITED = 3,
    SENTRY_RELAY_RESULT_ERROR = 4,
} sentry_relay_result_t;

/** One HTTP request, as the relay describes it. Nothing here is Sentry-specific. */
typedef struct {
    const char *url;
    /** Value for `X-Sentry-Auth`. Named generically on the wire — the host just copies it. */
    const char *auth;
    const char *content_type;
    const uint8_t *body;
    size_t body_len;
} sentry_relay_request_t;

/** Total bytes the DATA frames will carry: url + auth + content_type + body. */
size_t sentry_relay_stream_len(const sentry_relay_request_t *request);

/**
 * Copy up to `cap` bytes of the logical stream starting at `offset`.
 *
 * The stream is never materialised — this reads straight out of the caller's four buffers,
 * so relaying a 2 KB envelope costs one chunk-sized buffer rather than a second copy of the
 * whole request.
 */
size_t sentry_relay_stream_read(
    const sentry_relay_request_t *request, size_t offset, uint8_t *out, size_t cap);

/**
 * Encode the BEGIN frame. Returns bytes written, or 0 if `cap` is too small or the request
 * is too large for the protocol's 16-bit offsets.
 */
size_t sentry_relay_encode_begin(
    uint8_t *out, size_t cap, uint8_t request_id, const sentry_relay_request_t *request);

/**
 * Encode the DATA frame starting at `offset`, filling as much of `cap` as the stream has
 * left. Returns bytes written, or 0 when the stream is exhausted or `cap` is too small to
 * hold a header plus at least one payload byte.
 */
size_t sentry_relay_encode_data(uint8_t *out, size_t cap, uint8_t request_id,
    const sentry_relay_request_t *request, size_t offset);

/** Encode the END frame. Returns bytes written, or 0 if `cap` is too small. */
size_t sentry_relay_encode_end(uint8_t *out, size_t cap, uint8_t request_id);

/** What a parsed host frame turned out to be. */
typedef enum {
    SENTRY_RELAY_HOST_NONE = 0,
    SENTRY_RELAY_HOST_HELLO,
    SENTRY_RELAY_HOST_STATUS,
} sentry_relay_host_kind_t;

typedef struct {
    sentry_relay_host_kind_t kind;
    /** HELLO: the protocol version the host implements. */
    uint8_t protocol_version;
    /** HELLO: largest frame the host will accept, or 0 for "no opinion". */
    uint16_t max_chunk_bytes;
    /** STATUS: which request this answers. */
    uint8_t request_id;
    /** STATUS: the outcome, already translated into the SDK's own vocabulary. */
    sentry_response_t response;
} sentry_relay_host_frame_t;

/**
 * Parse one frame from the host.
 *
 * Returns false — leaving `out->kind` as `SENTRY_RELAY_HOST_NONE` — for anything short,
 * unknown or malformed. A companion app is untrusted input on a link anyone can speak, so
 * this rejects rather than guesses.
 */
bool sentry_relay_parse_host_frame(sentry_relay_host_frame_t *out, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_RELAY_H_INCLUDED */
