#include "frame_pure/ed25519.h"

#include "frame_pure/sha512.h"

#include <string.h>

/*
 * Field arithmetic over GF(2^255 - 19), 8 little-endian 32-bit limbs.
 *
 * Invariant: every fe produced by the functions below is canonical (< p), so
 * "is zero" and "is equal" are plain comparisons and no encoding step needs a
 * final reduction. Schoolbook multiplication plus two folds of 2^256 = 38 is
 * used instead of the usual radix-2^51 tricks: it is slower, but it is short,
 * obviously correct, and a signature check is a once-per-install event.
 */
typedef struct {
    uint32_t v[8];
} fe;

static const fe FE_ZERO = { { 0, 0, 0, 0, 0, 0, 0, 0 } };
static const fe FE_ONE  = { { 1, 0, 0, 0, 0, 0, 0, 0 } };
/* p = 2^255 - 19 = 0x7fff..ffed: top limb 0x7fffffff, the rest all ones. */
static const fe FE_P    = { { 0xffffffedu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                              0xffffffffu, 0xffffffffu, 0xffffffffu, 0x7fffffffu } };

/* L = 2^252 + 27742317777372353535851937790883648493 */
static const uint32_t SC_L[8] = {
    0x5cf5d3edu, 0x5812631au, 0xa2f79cd6u, 0x14def9deu, 0x00000000u, 0x00000000u, 0x00000000u, 0x10000000u
};

/* Base point B = (x, 4/5) with x even; encoded per RFC 8032. */
static const uint8_t BASE_ENCODED[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};

/* p - 2, (p - 5) / 8, (p - 1) / 4 — little-endian 32-byte exponents. */
static const uint8_t EXP_PM2[32]   = { 0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };
static const uint8_t EXP_PM5_8[32] = { 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f };
static const uint8_t EXP_PM1_4[32] = { 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f };

static fe  s_d;
static fe  s_2d;
static fe  s_sqrt_m1;
static int s_initialised;

/* ------------------------------------------------------------------ */
/* Field helpers                                                       */
/* ------------------------------------------------------------------ */

/* r >= p ? r - p : r */
static void fe_csub_p(fe *r)
{
    uint64_t borrow = 0;
    uint32_t t[8];
    for (int i = 0; i < 8; i++) {
        const int64_t s = (int64_t)r->v[i] - (int64_t)FE_P.v[i] - (int64_t)borrow;
        t[i]   = (uint32_t)s;
        borrow = (s < 0) ? 1u : 0u;
    }
    if (borrow == 0u) {
        memcpy(r->v, t, sizeof(t));
    }
}

static void fe_copy(fe *r, const fe *a)
{
    *r = *a;
}

static void fe_add(fe *r, const fe *a, const fe *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        const uint64_t s = (uint64_t)a->v[i] + b->v[i] + carry;
        r->v[i] = (uint32_t)s;
        carry   = s >> 32;
    }
    fe_csub_p(r);
}

static void fe_sub(fe *r, const fe *a, const fe *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        const int64_t s = (int64_t)a->v[i] - (int64_t)b->v[i] - (int64_t)borrow;
        r->v[i] = (uint32_t)s;
        borrow  = (s < 0) ? 1u : 0u;
    }
    if (borrow != 0u) {
        /* a < b: add p. */
        uint64_t carry = 0;
        for (int i = 0; i < 8; i++) {
            const uint64_t s = (uint64_t)r->v[i] + FE_P.v[i] + carry;
            r->v[i] = (uint32_t)s;
            carry   = s >> 32;
        }
    }
}

static void fe_neg(fe *r, const fe *a)
{
    fe_sub(r, &FE_ZERO, a);
}

static void fe_mul(fe *r, const fe *a, const fe *b)
{
    uint32_t t[16];
    memset(t, 0, sizeof(t));

    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            const uint64_t s = (uint64_t)a->v[i] * b->v[j] + t[i + j] + carry;
            t[i + j] = (uint32_t)s;
            carry    = s >> 32;
        }
        t[i + 8] = (uint32_t)carry;
    }

    /* Fold the high 256 bits: 2^256 == 38 (mod p). */
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        const uint64_t s = (uint64_t)t[i] + 38ull * t[i + 8] + carry;
        r->v[i] = (uint32_t)s;
        carry   = s >> 32;
    }

    /* Whatever carried out of the top limb is another multiple of 2^256. */
    carry *= 38ull;
    for (int pass = 0; pass < 2 && carry != 0u; pass++) {
        for (int i = 0; i < 8 && carry != 0u; i++) {
            const uint64_t s = (uint64_t)r->v[i] + carry;
            r->v[i] = (uint32_t)s;
            carry   = s >> 32;
        }
        carry *= 38ull;
    }
    if (carry != 0u) {
        r->v[0] += (uint32_t)carry;
    }

    /* 2^256 is slightly larger than 2p, so one subtraction is not quite
     * guaranteed to land below p. */
    fe_csub_p(r);
    fe_csub_p(r);
}

static void fe_sq(fe *r, const fe *a)
{
    fe_mul(r, a, a);
}

/* r = a * s for a small s (s < 2^20). */
static void fe_mul_small(fe *r, const fe *a, uint32_t s)
{
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        const uint64_t p = (uint64_t)a->v[i] * s + carry;
        r->v[i] = (uint32_t)p;
        carry   = p >> 32;
    }
    carry *= 38ull;
    for (int i = 0; i < 8 && carry != 0u; i++) {
        const uint64_t p = (uint64_t)r->v[i] + carry;
        r->v[i] = (uint32_t)p;
        carry   = p >> 32;
    }
    fe_csub_p(r);
}

static void fe_frombytes(fe *r, const uint8_t in[32])
{
    for (int i = 0; i < 8; i++) {
        r->v[i] = (uint32_t)in[i * 4] | ((uint32_t)in[i * 4 + 1] << 8) |
                  ((uint32_t)in[i * 4 + 2] << 16) | ((uint32_t)in[i * 4 + 3] << 24);
    }
    r->v[7] &= 0x7fffffffu;
    fe_csub_p(r);
}

static void fe_tobytes(const fe *a, uint8_t out[32])
{
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(a->v[i]);
        out[i * 4 + 1] = (uint8_t)(a->v[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(a->v[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(a->v[i] >> 24);
    }
}

static int fe_is_zero(const fe *a)
{
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) {
        acc |= a->v[i];
    }
    return (acc == 0u) ? 1 : 0;
}

static int fe_equal(const fe *a, const fe *b)
{
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) {
        acc |= a->v[i] ^ b->v[i];
    }
    return (acc == 0u) ? 1 : 0;
}

/* r = a^e for a 32-byte little-endian exponent e. */
static void fe_pow(fe *r, const fe *a, const uint8_t e[32])
{
    fe acc;
    fe_copy(&acc, &FE_ONE);

    int started = 0;
    for (int i = 255; i >= 0; i--) {
        if (started) {
            fe_sq(&acc, &acc);
        }
        const int bit = (e[i >> 3] >> (i & 7)) & 1;
        if (bit) {
            if (started) {
                fe_mul(&acc, &acc, a);
            } else {
                fe_copy(&acc, a);
                started = 1;
            }
        }
    }
    if (!started) {
        fe_copy(&acc, &FE_ONE);
    }
    *r = acc;
}

static void fe_invert(fe *r, const fe *a)
{
    fe_pow(r, a, EXP_PM2);
}

/* ------------------------------------------------------------------ */
/* Scalars                                                             */
/* ------------------------------------------------------------------ */

static int sc_ge(const uint32_t a[8], const uint32_t b[8])
{
    for (int i = 7; i >= 0; i--) {
        if (a[i] != b[i]) {
            return a[i] > b[i] ? 1 : 0;
        }
    }
    return 1;
}

static void sc_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        const int64_t s = (int64_t)a[i] - (int64_t)b[i] - (int64_t)borrow;
        r[i]    = (uint32_t)s;
        borrow  = (s < 0) ? 1u : 0u;
    }
}

/* Reduces a 512-bit little-endian integer modulo L by long division, one bit
 * at a time. The running remainder stays below 2L < 2^254, so it never
 * overflows eight limbs. */
static void sc_reduce_wide(uint32_t out[8], const uint32_t in[16])
{
    uint32_t r[8] = { 0 };
    for (int i = 511; i >= 0; i--) {
        uint32_t carry = 0;
        for (int j = 0; j < 8; j++) {
            const uint32_t next = r[j] >> 31;
            r[j]   = (r[j] << 1) | carry;
            carry  = next;
        }
        r[0] |= (in[i >> 5] >> (i & 31)) & 1u;
        if (sc_ge(r, SC_L)) {
            sc_sub(r, r, SC_L);
        }
    }
    memcpy(out, r, sizeof(r));
}

static int sc_from_bytes(uint32_t out[8], const uint8_t in[32])
{
    for (int i = 0; i < 8; i++) {
        out[i] = (uint32_t)in[i * 4] | ((uint32_t)in[i * 4 + 1] << 8) |
                 ((uint32_t)in[i * 4 + 2] << 16) | ((uint32_t)in[i * 4 + 3] << 24);
    }
    /* Must be canonical: 0 <= s < L. */
    return sc_ge(out, SC_L) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Group                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    fe X, Y, Z, T;
} ge;

/* add-2008-hwcd-3, unified (valid for P == Q as well). */
static void ge_add(ge *r, const ge *p, const ge *q)
{
    fe a, b, c, d, e, f, g, h, t0, t1;

    fe_sub(&t0, &p->Y, &p->X);
    fe_sub(&t1, &q->Y, &q->X);
    fe_mul(&a, &t0, &t1);                 /* A = (Y1-X1)(Y2-X2) */

    fe_add(&t0, &p->Y, &p->X);
    fe_add(&t1, &q->Y, &q->X);
    fe_mul(&b, &t0, &t1);                 /* B = (Y1+X1)(Y2+X2) */

    fe_mul(&t0, &p->T, &q->T);
    fe_mul(&c, &t0, &s_2d);               /* C = T1 * 2d * T2 */

    fe_mul(&t0, &p->Z, &q->Z);
    fe_add(&d, &t0, &t0);                 /* D = 2 Z1 Z2 */

    fe_sub(&e, &b, &a);
    fe_sub(&f, &d, &c);
    fe_add(&g, &d, &c);
    fe_add(&h, &b, &a);

    fe_mul(&r->X, &e, &f);
    fe_mul(&r->Y, &g, &h);
    fe_mul(&r->T, &e, &h);
    fe_mul(&r->Z, &f, &g);
}

static void ge_double(ge *r, const ge *p)
{
    ge_add(r, p, p);
}

static void ge_neg(ge *r, const ge *p)
{
    fe_neg(&r->X, &p->X);
    fe_copy(&r->Y, &p->Y);
    fe_copy(&r->Z, &p->Z);
    fe_neg(&r->T, &p->T);
}

static void ge_identity(ge *r)
{
    fe_copy(&r->X, &FE_ZERO);
    fe_copy(&r->Y, &FE_ONE);
    fe_copy(&r->Z, &FE_ONE);
    fe_copy(&r->T, &FE_ZERO);
}

static int ge_is_identity(const ge *p)
{
    return fe_is_zero(&p->X) && fe_is_zero(&p->T) && fe_equal(&p->Y, &p->Z);
}

/* Left-to-right double-and-add. Starts from the top set bit so the identity
 * never has to be doubled (the unified formula does not preserve it). */
static void ge_scalarmult(ge *r, const uint32_t s[8], const ge *p)
{
    int top = -1;
    for (int i = 255; i >= 0; i--) {
        if ((s[i >> 5] >> (i & 31)) & 1u) {
            top = i;
            break;
        }
    }
    if (top < 0 || ge_is_identity(p)) {
        ge_identity(r);
        return;
    }

    ge acc = *p;
    for (int i = top - 1; i >= 0; i--) {
        ge_double(&acc, &acc);
        if ((s[i >> 5] >> (i & 31)) & 1u) {
            ge_add(&acc, &acc, p);
        }
    }
    *r = acc;
}

static int ge_frombytes(ge *p, const uint8_t in[32])
{
    fe y;
    fe_frombytes(&y, in);
    const uint32_t sign = in[31] >> 7;

    fe y2, u, v, v3, v7, t, x, vxx, check;
    fe_sq(&y2, &y);
    fe_sub(&u, &y2, &FE_ONE);                 /* u = y^2 - 1          */
    fe_mul(&v, &y2, &s_d);
    fe_add(&v, &v, &FE_ONE);                  /* v = d y^2 + 1        */
    fe_sq(&v3, &v);
    fe_mul(&v3, &v3, &v);                     /* v^3                  */
    fe_sq(&v7, &v3);
    fe_mul(&v7, &v7, &v);                     /* v^7                  */
    fe_mul(&t, &u, &v7);                      /* u v^7                */
    fe_pow(&t, &t, EXP_PM5_8);                /* (u v^7)^((p-5)/8)    */
    fe_mul(&x, &t, &v3);
    fe_mul(&x, &x, &u);                       /* x = u v^3 (u v^7)^.. */
    fe_sq(&vxx, &x);
    fe_mul(&vxx, &vxx, &v);                   /* v x^2                */

    fe_sub(&check, &vxx, &u);
    if (!fe_is_zero(&check)) {
        fe_add(&check, &vxx, &u);
        if (!fe_is_zero(&check)) {
            return -1;                        /* not on the curve     */
        }
        fe_mul(&x, &x, &s_sqrt_m1);
    }

    if ((uint32_t)(x.v[0] & 1u) != sign) {
        fe_neg(&x, &x);
    }

    fe_copy(&p->X, &x);
    fe_copy(&p->Y, &y);
    fe_copy(&p->Z, &FE_ONE);
    fe_mul(&p->T, &x, &y);
    return 0;
}

static void ge_tobytes(const ge *p, uint8_t out[32])
{
    fe zinv, x, y;
    fe_invert(&zinv, &p->Z);
    fe_mul(&x, &p->X, &zinv);
    fe_mul(&y, &p->Y, &zinv);
    fe_tobytes(&y, out);
    out[31] ^= (uint8_t)((x.v[0] & 1u) << 7);
}

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

static void lazy_init(void)
{
    if (s_initialised) {
        return;
    }

    /* d = -121665 / 121666 */
    fe num, den, inv;
    fe_mul_small(&num, &FE_ONE, 121665u);
    fe_mul_small(&den, &FE_ONE, 121666u);
    fe_invert(&inv, &den);
    fe_mul(&s_d, &num, &inv);
    fe_neg(&s_d, &s_d);

    fe_add(&s_2d, &s_d, &s_d);

    /* sqrt(-1) = 2^((p-1)/4) — valid because p == 5 (mod 8). */
    fe two;
    fe_mul_small(&two, &FE_ONE, 2u);
    fe_pow(&s_sqrt_m1, &two, EXP_PM1_4);

    s_initialised = 1;
}

/* ------------------------------------------------------------------ */

void frame_ed25519_verify_begin(frame_ed25519_verify_ctx_t *ctx,
                                const uint8_t sig[FRAME_ED25519_SIGNATURE_LEN],
                                const uint8_t pub[FRAME_ED25519_PUBLIC_KEY_LEN])
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->failed = 1;
    if (sig == NULL || pub == NULL) {
        return;
    }
    memcpy(ctx->sig, sig, FRAME_ED25519_SIGNATURE_LEN);
    memcpy(ctx->pub, pub, FRAME_ED25519_PUBLIC_KEY_LEN);

    /* Reject up front what end() would reject anyway: a non-canonical S or a
     * key that is not a curve point. Doing it here means update() never does
     * work for a signature that cannot verify. */
    lazy_init();
    uint32_t s[8];
    ge A;
    if (sc_from_bytes(s, sig + 32) != 0 || ge_frombytes(&A, pub) != 0) {
        return;
    }

    frame_sha512_init(&ctx->sha);
    frame_sha512_update(&ctx->sha, sig, 32);
    frame_sha512_update(&ctx->sha, pub, 32);
    ctx->failed = 0;
}

void frame_ed25519_verify_update(frame_ed25519_verify_ctx_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL || ctx->failed || (data == NULL && len > 0u)) {
        return;
    }
    if (len > 0u) {
        frame_sha512_update(&ctx->sha, data, len);
    }
}

int frame_ed25519_verify_end(frame_ed25519_verify_ctx_t *ctx)
{
    if (ctx == NULL || ctx->failed) {
        return -1;
    }

    ge A;
    if (ge_frombytes(&A, ctx->pub) != 0) {
        return -1;
    }
    uint32_t s[8];
    if (sc_from_bytes(s, ctx->sig + 32) != 0) {
        return -1;
    }

    uint8_t digest[FRAME_SHA512_DIGEST_LEN];
    frame_sha512_final(&ctx->sha, digest);

    uint32_t wide[16];
    for (int i = 0; i < 16; i++) {
        wide[i] = (uint32_t)digest[i * 4] | ((uint32_t)digest[i * 4 + 1] << 8) |
                  ((uint32_t)digest[i * 4 + 2] << 16) | ((uint32_t)digest[i * 4 + 3] << 24);
    }
    uint32_t h[8];
    sc_reduce_wide(h, wide);

    /* R' = [s]B - [h]A */
    ge B, sB, hA, neg_hA, R;
    if (ge_frombytes(&B, BASE_ENCODED) != 0) {
        return -1;
    }
    ge_scalarmult(&sB, s, &B);
    ge_scalarmult(&hA, h, &A);
    ge_neg(&neg_hA, &hA);
    ge_add(&R, &sB, &neg_hA);

    uint8_t encoded[32];
    ge_tobytes(&R, encoded);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (uint8_t)(encoded[i] ^ ctx->sig[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
    return (diff == 0u) ? 0 : -1;
}

int frame_ed25519_verify(const uint8_t sig[FRAME_ED25519_SIGNATURE_LEN],
                         const uint8_t pub[FRAME_ED25519_PUBLIC_KEY_LEN],
                         const void *msg, size_t msg_len)
{
    if (sig == NULL || pub == NULL || (msg == NULL && msg_len > 0u)) {
        return -1;
    }
    frame_ed25519_verify_ctx_t ctx;
    frame_ed25519_verify_begin(&ctx, sig, pub);
    if (msg_len > 0u) {
        frame_ed25519_verify_update(&ctx, msg, msg_len);
    }
    return frame_ed25519_verify_end(&ctx);
}
