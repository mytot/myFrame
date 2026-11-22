#include "frame_pure/sha512.h"

#include <string.h>

/* Round constants: fractional parts of the cube roots of the first 80 primes.
 * The table was generated with integer arithmetic (no floating point) and
 * checked against FIPS 180-4 by tests/test_pure.c. */
static const uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull,
    0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull,
    0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull,
    0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull,
    0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull,
    0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull,
    0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull,
    0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull,
    0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull,
    0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull,
    0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static uint64_t rotr64(uint64_t x, unsigned n)
{
    return (x >> n) | (x << (64u - n));
}

/* A SHA-512 block is 16 big-endian 64-bit words. */
static void load_be64(const uint8_t *p, uint64_t out[16])
{
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) {
            v = (v << 8) | p[i * 8 + j];
        }
        out[i] = v;
    }
}

static void store_be64(uint64_t v, uint8_t out[8])
{
    for (int j = 7; j >= 0; j--) {
        out[j] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}

static void compress(uint64_t state[8], const uint8_t block[FRAME_SHA512_BLOCK_LEN])
{
    uint64_t w[80];
    load_be64(block, w);
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 80; i++) {
        const uint64_t S1  = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        const uint64_t ch  = (e & f) ^ (~e & g);
        const uint64_t t1  = h + S1 + ch + K[i] + w[i];
        const uint64_t S0  = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2  = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void frame_sha512_init(frame_sha512_ctx_t *ctx)
{
    /* Fractional parts of the square roots of the first 8 primes. */
    ctx->state[0] = 0x6a09e667f3bcc908ull;
    ctx->state[1] = 0xbb67ae8584caa73bull;
    ctx->state[2] = 0x3c6ef372fe94f82bull;
    ctx->state[3] = 0xa54ff53a5f1d36f1ull;
    ctx->state[4] = 0x510e527fade682d1ull;
    ctx->state[5] = 0x9b05688c2b3e6c1full;
    ctx->state[6] = 0x1f83d9abfb41bd6bull;
    ctx->state[7] = 0x5be0cd19137e2179ull;
    ctx->len_lo   = 0;
    ctx->len_hi   = 0;
    ctx->buflen   = 0;
}

void frame_sha512_update(frame_sha512_ctx_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL || data == NULL) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data;

    const uint64_t add = (uint64_t)len;
    ctx->len_lo += add;
    if (ctx->len_lo < add) {
        ctx->len_hi++;
    }

    if (ctx->buflen > 0u) {
        const size_t need = FRAME_SHA512_BLOCK_LEN - ctx->buflen;
        const size_t take = (len < need) ? len : need;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p   += take;
        len -= take;
        if (ctx->buflen == FRAME_SHA512_BLOCK_LEN) {
            compress(ctx->state, ctx->buf);
            ctx->buflen = 0;
        }
    }

    while (len >= FRAME_SHA512_BLOCK_LEN) {
        compress(ctx->state, p);
        p   += FRAME_SHA512_BLOCK_LEN;
        len -= FRAME_SHA512_BLOCK_LEN;
    }

    if (len > 0u) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void frame_sha512_final(frame_sha512_ctx_t *ctx, uint8_t out[FRAME_SHA512_DIGEST_LEN])
{
    if (ctx == NULL || out == NULL) {
        return;
    }

    /* Pad the remaining buffer directly so the running length is untouched:
     * the length that goes into the trailer is the *message* length. */
    const uint64_t bits_lo = ctx->len_lo << 3;
    const uint64_t bits_hi = (ctx->len_hi << 3) | (ctx->len_lo >> 61);

    const size_t rem = ctx->buflen;
    ctx->buf[rem] = 0x80u;
    size_t pad_to = (rem < 112u) ? 112u : FRAME_SHA512_BLOCK_LEN;
    if (rem + 1u < pad_to) {
        memset(ctx->buf + rem + 1u, 0, pad_to - rem - 1u);
    }
    if (pad_to == FRAME_SHA512_BLOCK_LEN) {
        compress(ctx->state, ctx->buf);
        memset(ctx->buf, 0, 112u);
    }

    memset(ctx->buf + 112u, 0, 16u);
    store_be64(bits_hi, ctx->buf + 112u);
    store_be64(bits_lo, ctx->buf + 120u);
    compress(ctx->state, ctx->buf);

    for (int i = 0; i < 8; i++) {
        store_be64(ctx->state[i], out + i * 8);
    }

    memset(ctx, 0, sizeof(*ctx));
}

void frame_sha512(const void *data, size_t len, uint8_t out[FRAME_SHA512_DIGEST_LEN])
{
    frame_sha512_ctx_t ctx;
    frame_sha512_init(&ctx);
    frame_sha512_update(&ctx, data, len);
    frame_sha512_final(&ctx, out);
}
