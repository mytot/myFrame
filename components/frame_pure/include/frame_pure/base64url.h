/*
 * frame_pure/base64url.h — unpadded base64url encode/decode (RFC 4648 §5).
 *
 * Used for signatures and digests inside manifests. Unpadded, because that is
 * what JWS and most tooling emit for Ed25519.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded length of a base64url string (ignoring padding), or -1 if malformed. */
long frame_b64url_decoded_len(const char *s);

/* Returns bytes written, or -1 on malformed input / buffer overflow. */
long frame_b64url_decode(const char *s, uint8_t *out, size_t out_cap);

/* Returns characters written (excluding the NUL), or -1 on overflow. */
long frame_b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
