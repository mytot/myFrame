/*
 * frame_pure/sha512.h — self-contained SHA-512 (FIPS 180-4).
 *
 * Exists for one reason: Ed25519 is defined over SHA-512, and the package
 * verifier must run before any cryptographic service is initialised and must
 * be host-testable against published vectors. See frame_pure/ed25519.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_SHA512_DIGEST_LEN 64u
#define FRAME_SHA512_BLOCK_LEN  128u

typedef struct {
    uint64_t state[8];
    uint64_t len_lo;    /* bytes processed, low 64 bits  */
    uint64_t len_hi;    /* bytes processed, high 64 bits */
    uint8_t  buf[FRAME_SHA512_BLOCK_LEN];
    size_t   buflen;
} frame_sha512_ctx_t;

void frame_sha512_init(frame_sha512_ctx_t *ctx);
void frame_sha512_update(frame_sha512_ctx_t *ctx, const void *data, size_t len);
void frame_sha512_final(frame_sha512_ctx_t *ctx, uint8_t out[FRAME_SHA512_DIGEST_LEN]);

/* Convenience one-shot. */
void frame_sha512(const void *data, size_t len, uint8_t out[FRAME_SHA512_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif
