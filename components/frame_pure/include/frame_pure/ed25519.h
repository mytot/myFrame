/*
 * frame_pure/ed25519.h — Ed25519 signature *verification* (RFC 8032).
 *
 * Why in-repo rather than PSA Crypto: the package verifier has to run on a
 * device whose crypto stack may not expose Ed25519, and it has to be
 * host-testable against the RFC 8032 vectors before it ever touches a device.
 * This is verification only — the private key never exists on the device.
 *
 * NOT constant-time. That is acceptable here because every input (signature,
 * public key, message) is public; nothing secret is processed.
 */
#pragma once

#include "frame_pure/sha512.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_ED25519_PUBLIC_KEY_LEN 32u
#define FRAME_ED25519_SIGNATURE_LEN  64u

/* Returns 0 when the signature is valid, -1 otherwise. */
int frame_ed25519_verify(const uint8_t sig[FRAME_ED25519_SIGNATURE_LEN],
                         const uint8_t pub[FRAME_ED25519_PUBLIC_KEY_LEN],
                         const void *msg, size_t msg_len);

/*
 * Streaming form for messages that do not fit in RAM (a package payload is
 * fed in from flash in chunks). Same result as frame_ed25519_verify().
 *
 *   frame_ed25519_verify_begin(&ctx, sig, pub);
 *   while (chunk) frame_ed25519_verify_update(&ctx, data, len);
 *   ok = frame_ed25519_verify_end(&ctx) == 0;
 */
typedef struct {
    frame_sha512_ctx_t sha;
    uint8_t            sig[FRAME_ED25519_SIGNATURE_LEN];
    uint8_t            pub[FRAME_ED25519_PUBLIC_KEY_LEN];
    int                failed;
} frame_ed25519_verify_ctx_t;

void frame_ed25519_verify_begin(frame_ed25519_verify_ctx_t *ctx,
                                const uint8_t sig[FRAME_ED25519_SIGNATURE_LEN],
                                const uint8_t pub[FRAME_ED25519_PUBLIC_KEY_LEN]);
void frame_ed25519_verify_update(frame_ed25519_verify_ctx_t *ctx, const void *data, size_t len);
int  frame_ed25519_verify_end(frame_ed25519_verify_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
