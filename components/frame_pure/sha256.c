#include "frame_pure/sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void transform(frame_sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void frame_sha256_init(frame_sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void frame_sha256_update(frame_sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->bitlen += (uint64_t)len * 8u;

    while (len > 0) {
        size_t take = FRAME_SHA256_BLOCK_LEN - ctx->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == FRAME_SHA256_BLOCK_LEN) {
            transform(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void frame_sha256_final(frame_sha256_ctx_t *ctx, uint8_t out[FRAME_SHA256_DIGEST_LEN])
{
    const uint64_t bits = ctx->bitlen;
    size_t i = ctx->buflen;

    /* 0x80 terminator. */
    ctx->buf[i++] = 0x80;

    /* Not enough room for the 8 length bytes: flush and start a fresh block. */
    if (i > 56u) {
        while (i < FRAME_SHA256_BLOCK_LEN) {
            ctx->buf[i++] = 0x00;
        }
        transform(ctx, ctx->buf);
        i = 0;
    }
    while (i < 56u) {
        ctx->buf[i++] = 0x00;
    }

    /* Message length, big-endian, in bits. */
    for (int k = 0; k < 8; k++) {
        ctx->buf[56 + k] = (uint8_t)(bits >> (56 - 8 * k));
    }
    transform(ctx, ctx->buf);

    for (int k = 0; k < 8; k++) {
        out[k * 4]     = (uint8_t)(ctx->state[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(ctx->state[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(ctx->state[k] >> 8);
        out[k * 4 + 3] = (uint8_t)(ctx->state[k]);
    }
}

void frame_sha256(const void *data, size_t len, uint8_t out[FRAME_SHA256_DIGEST_LEN])
{
    frame_sha256_ctx_t ctx;
    frame_sha256_init(&ctx);
    frame_sha256_update(&ctx, data, len);
    frame_sha256_final(&ctx, out);
}

void frame_sha256_hex(const uint8_t digest[FRAME_SHA256_DIGEST_LEN], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < FRAME_SHA256_DIGEST_LEN; i++) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0Fu];
    }
    out[64] = '\0';
}
