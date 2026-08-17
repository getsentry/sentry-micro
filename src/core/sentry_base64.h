/**
 * Base64 encoding.
 *
 * Needed because a Sentry envelope is ndjson — it *contains* newlines — and the relay
 * protocols carry it over links that are line-framed and shared with human-readable log
 * output. Encoding turns the payload into one newline-free token, which removes every
 * escaping and framing question at the cost of a third more bytes.
 *
 * Portable C with no allocation, so it is host-testable and equally usable by the serial
 * relay, the BLE relay, and eventually attachments.
 */
#ifndef SENTRY_MICRO_BASE64_H_INCLUDED
#define SENTRY_MICRO_BASE64_H_INCLUDED

#include "sentry_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of output an encode of `len` bytes needs, excluding the terminating NUL. */
#define SENTRY_BASE64_ENCODED_LEN(len) (4 * (((len) + 2) / 3))

/**
 * Encode `len` bytes of `data` into `out` as standard base64 with `=` padding.
 *
 * Returns the number of characters the encoding needs, excluding the NUL. If that is >=
 * `out_cap`, nothing is written and `out` is left empty — a truncated base64 string would
 * decode to a truncated envelope, which is worse than no envelope.
 */
size_t sentry_base64_encode(char *out, size_t out_cap, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_BASE64_H_INCLUDED */
