#include "frame_pure/base64url.h"

#include <string.h>

static const char ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int dec_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;  /* tolerate standard base64 too */
    if (c == '_' || c == '/') return 63;
    return -1;
}

long frame_b64url_decoded_len(const char *s)
{
    if (s == NULL) {
        return -1;
    }
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '=') {
        n--;
    }
    if (n == 0) {
        return 0;
    }
    /* Only the final quantum may be short; anything else is malformed. */
    size_t rem = n % 4u;
    if (rem == 1) {
        return -1;
    }
    long bytes = (long)(n / 4u) * 3;
    if (rem == 2) {
        bytes += 1;
    } else if (rem == 3) {
        bytes += 2;
    }
    return bytes;
}

long frame_b64url_decode(const char *s, uint8_t *out, size_t out_cap)
{
    long total = frame_b64url_decoded_len(s);
    if (total < 0 || (size_t)total > out_cap) {
        return -1;
    }

    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '=') {
        n--;
    }

    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < n; i++) {
        int d = dec_char(s[i]);
        if (d < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) {
                return -1;
            }
            out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return (long)o;
}

long frame_b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    if (in == NULL || out == NULL) {
        return -1;
    }
    size_t need = (in_len * 4u + 2u) / 3u;
    if (need + 1u > out_cap) {
        return -1;
    }

    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = ENC[(v >> 18) & 63u];
        out[o++] = ENC[(v >> 12) & 63u];
        out[o++] = ENC[(v >> 6) & 63u];
        out[o++] = ENC[v & 63u];
        i += 3u;
    }
    size_t rem = in_len - i;
    if (rem == 1u) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = ENC[(v >> 18) & 63u];
        out[o++] = ENC[(v >> 12) & 63u];
    } else if (rem == 2u) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = ENC[(v >> 18) & 63u];
        out[o++] = ENC[(v >> 12) & 63u];
        out[o++] = ENC[(v >> 6) & 63u];
    }
    out[o] = '\0';
    return (long)o;
}
