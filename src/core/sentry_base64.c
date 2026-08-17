#include "sentry_base64.h"

static const char k_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                 "0123456789+/";

size_t sentry_base64_encode(char *out, size_t out_cap, const uint8_t *data, size_t len)
{
    size_t needed = SENTRY_BASE64_ENCODED_LEN(len);
    if (!out || out_cap == 0) {
        return needed;
    }
    if (needed + 1 > out_cap || (!data && len > 0)) {
        out[0] = '\0';
        return needed;
    }

    size_t o = 0;
    size_t i = 0;
    /* Whole 3-byte groups: 24 bits in, four 6-bit symbols out. */
    for (; i + 3 <= len; i += 3) {
        uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = k_alphabet[(triple >> 18) & 0x3f];
        out[o++] = k_alphabet[(triple >> 12) & 0x3f];
        out[o++] = k_alphabet[(triple >> 6) & 0x3f];
        out[o++] = k_alphabet[triple & 0x3f];
    }

    /* Tail of 1 or 2 bytes, zero-extended to 24 bits and padded with '=' so the length
     * stays a multiple of four and a decoder can recover the original byte count. */
    size_t remaining = len - i;
    if (remaining > 0) {
        uint32_t triple = (uint32_t)data[i] << 16;
        if (remaining == 2) {
            triple |= (uint32_t)data[i + 1] << 8;
        }
        out[o++] = k_alphabet[(triple >> 18) & 0x3f];
        out[o++] = k_alphabet[(triple >> 12) & 0x3f];
        out[o++] = remaining == 2 ? k_alphabet[(triple >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }

    out[o] = '\0';
    return needed;
}
