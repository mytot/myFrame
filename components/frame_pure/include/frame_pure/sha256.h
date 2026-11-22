/*
 * frame_pure/sha256.h — self-contained SHA-256 (FIPS 180-4).
 *
 * Implemented in-repo rather than pulled from mbedTLS for two reasons: it must
 * be host-testable against published vectors, and it must be available before
 * PSA Crypto is initialised (manifest verification can run very early).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_SHA256_DIGEST_LEN 32u
#define FRAME_SHA256_BLOCK_LEN  64u

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[FRAME_SHA256_BLOCK_LEN];
    size_t   buflen;
} frame_sha256_ctx_t;

void frame_sha256_init(frame_sha256_ctx_t *ctx);
void frame_sha256_update(frame_sha256_ctx_t *ctx, const void *data, size_t len);
void frame_sha256_final(frame_sha256_ctx_t *ctx, uint8_t out[FRAME_SHA256_DIGEST_LEN]);

/* Convenience one-shot. */
void frame_sha256(const void *data, size_t len, uint8_t out[FRAME_SHA256_DIGEST_LEN]);

/* Lowercase hex, writes 65 bytes including the NUL. */
void frame_sha256_hex(const uint8_t digest[FRAME_SHA256_DIGEST_LEN], char out[65]);

#ifdef __cplusplus
}
#endif
