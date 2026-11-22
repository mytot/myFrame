/*
 * tests/test_crypto.c — SHA-512 and Ed25519 verification against published
 * vectors (FIPS 180-4, RFC 8032 §7.1) plus the failure modes the store must
 * reject. Host build only; no ESP-IDF headers involved.
 */
#include "frame_pure/ed25519.h"
#include "frame_pure/sha512.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                  \
    } while (0)

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(const char *hex, uint8_t *out, size_t cap, size_t *len_out)
{
    size_t n = strlen(hex);
    if ((n & 1u) != 0u || n / 2u > cap) {
        return -1;
    }
    for (size_t i = 0; i < n / 2u; i++) {
        const int hi = hexval(hex[i * 2u]);
        const int lo = hexval(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *len_out = n / 2u;
    return 0;
}

static void hexdump(const uint8_t *b, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2u]     = d[b[i] >> 4];
        out[i * 2u + 1u] = d[b[i] & 15u];
    }
    out[n * 2u] = '\0';
}

static void test_sha512_vector(const char *name, const char *msg, const char *expect_hex)
{
    uint8_t digest[64];
    frame_sha512(msg, strlen(msg), digest);
    char got[129];
    hexdump(digest, 64, got);
    CHECK(strcmp(got, expect_hex) == 0);
    printf("  sha512(%s) %s\n", name, strcmp(got, expect_hex) == 0 ? "ok" : got);
}

static void test_sha512_multiblock(void)
{
    /* 200 bytes spans two blocks with a 72-byte tail; compared against the
     * same computation via the streaming API. */
    uint8_t buf[200];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 7u);
    }
    uint8_t a[64], b[64];
    frame_sha512(buf, sizeof(buf), a);

    frame_sha512_ctx_t ctx;
    frame_sha512_init(&ctx);
    for (size_t off = 0; off < sizeof(buf); off += 13u) {
        const size_t n = (sizeof(buf) - off < 13u) ? sizeof(buf) - off : 13u;
        frame_sha512_update(&ctx, buf + off, n);
    }
    frame_sha512_final(&ctx, b);
    CHECK(memcmp(a, b, 64) == 0);
    printf("  sha512 streaming == one-shot: %s\n", memcmp(a, b, 64) == 0 ? "ok" : "FAIL");
}

typedef struct {
    const char *name;
    const char *sk;   /* not used by the verifier; kept for reference */
    const char *pk;
    const char *msg;  /* hex, may be "" */
    const char *sig;
} eddsa_vector_t;

static const eddsa_vector_t k_vectors[] = {
    { "rfc8032-1 empty",
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "rfc8032-2 one byte",
      "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "python-signed sample",
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "4672616d654f53207061636b6167652074657374206d657373616765",
      "b03ddb45d6b1a811454fd1233ebb8d9bebccdb73b5c91e07785278d26363796307ffae34591d5ab3601fcba516defdf69bbaaf92a93f5ed8808f7ea7e8dc3903" },
    { "rfc8032-3 two bytes",
      "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
};

static void test_ed25519_vectors(void)
{
    uint8_t pk[32], msg[64], sig[64];
    size_t msg_len = 0;

    for (size_t i = 0; i < sizeof(k_vectors) / sizeof(k_vectors[0]); i++) {
        const eddsa_vector_t *v = &k_vectors[i];
        size_t n = 0;
        CHECK(unhex(v->pk, pk, sizeof(pk), &n) == 0 && n == 32);
        CHECK(unhex(v->sig, sig, sizeof(sig), &n) == 0 && n == 64);
        if (v->msg[0] != '\0') {
            CHECK(unhex(v->msg, msg, sizeof(msg), &msg_len) == 0);
        } else {
            msg_len = 0;
        }
        const int rc = frame_ed25519_verify(sig, pk, msg, msg_len);
        CHECK(rc == 0);
        printf("  ed25519 %s: %s\n", v->name, rc == 0 ? "ok" : "REJECTED");
    }
}

static void test_ed25519_rejects(void)
{
    uint8_t pk[32], msg[1] = { 0x72 }, sig[64];
    size_t n = 0;
    CHECK(unhex(k_vectors[1].pk, pk, sizeof(pk), &n) == 0);
    CHECK(unhex(k_vectors[1].sig, sig, sizeof(sig), &n) == 0);

    /* Sanity. */
    CHECK(frame_ed25519_verify(sig, pk, msg, 1) == 0);

    /* Tampered message. */
    msg[0] = 0x73;
    CHECK(frame_ed25519_verify(sig, pk, msg, 1) != 0);
    msg[0] = 0x72;

    /* Tampered signature (last byte of S). */
    uint8_t bad[64];
    memcpy(bad, sig, 64);
    bad[63] ^= 0x01;
    CHECK(frame_ed25519_verify(bad, pk, msg, 1) != 0);

    /* Wrong message length. */
    CHECK(frame_ed25519_verify(sig, pk, msg, 0) != 0);

    /* Wrong public key. */
    uint8_t other[32];
    CHECK(unhex(k_vectors[0].pk, other, sizeof(other), &n) == 0);
    CHECK(frame_ed25519_verify(sig, other, msg, 1) != 0);

    /* Public key not on the curve (y = 2 is not a valid curve point's y). */
    uint8_t notcurve[32] = { 0 };
    notcurve[0] = 2;
    CHECK(frame_ed25519_verify(sig, notcurve, msg, 1) != 0);

    /* Non-canonical S (S + L must not verify). */
    static const uint8_t L_BYTES[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    };
    memcpy(bad, sig, 64);
    uint32_t carry = 0;
    for (int i = 0; i < 32; i++) {
        const uint32_t sum = (uint32_t)bad[32 + i] + L_BYTES[i] + carry;
        bad[32 + i] = (uint8_t)sum;
        carry = sum >> 8;
    }
    CHECK(frame_ed25519_verify(bad, pk, msg, 1) != 0);

    /* NULL handling. */
    CHECK(frame_ed25519_verify(NULL, pk, msg, 1) == -1);
    CHECK(frame_ed25519_verify(sig, NULL, msg, 1) == -1);
    printf("  ed25519 rejections: ok\n");
}

static void test_ed25519_streaming(void)
{
    uint8_t pk[32], sig[64], msg[64];
    size_t n = 0, msg_len = 0;
    CHECK(unhex(k_vectors[3].pk, pk, sizeof(pk), &n) == 0);
    CHECK(unhex(k_vectors[3].sig, sig, sizeof(sig), &n) == 0);
    CHECK(unhex(k_vectors[3].msg, msg, sizeof(msg), &msg_len) == 0);

    /* Same message, fed in irregular chunks — the shape a package payload
     * arrives in when it is streamed out of flash. */
    static const size_t chunks[] = { 1, 7, 3, 13, 2 };
    frame_ed25519_verify_ctx_t ctx;
    frame_ed25519_verify_begin(&ctx, sig, pk);
    size_t off = 0;
    for (size_t i = 0; off < msg_len; i++) {
        size_t take = chunks[i % 5u];
        if (off + take > msg_len) {
            take = msg_len - off;
        }
        frame_ed25519_verify_update(&ctx, msg + off, take);
        off += take;
    }
    CHECK(frame_ed25519_verify_end(&ctx) == 0);

    /* Streamed with the last byte flipped. */
    msg[msg_len - 1u] ^= 0x01;
    frame_ed25519_verify_begin(&ctx, sig, pk);
    frame_ed25519_verify_update(&ctx, msg, msg_len);
    CHECK(frame_ed25519_verify_end(&ctx) != 0);
    msg[msg_len - 1u] ^= 0x01;

    /* A non-canonical S must fail in begin(), before any data is hashed. */
    uint8_t bad[64];
    memcpy(bad, sig, 64);
    bad[63] |= 0x80;
    frame_ed25519_verify_begin(&ctx, bad, pk);
    frame_ed25519_verify_update(&ctx, msg, msg_len);
    CHECK(frame_ed25519_verify_end(&ctx) != 0);
    printf("  ed25519 streaming: ok\n");
}

int main(void)
{
    printf("test_crypto\n");

    test_sha512_vector("empty", "",
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    test_sha512_vector("abc", "abc",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    test_sha512_vector("448-bit", "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    test_sha512_multiblock();
    {
        /* 128 zero bytes: two blocks, exercises the length trailer in the
         * second block. The first implementation missed loading words 8..15
         * of each block and only the zero-length vector passed by luck. */
        uint8_t z[128] = { 0 };
        uint8_t d[64];
        char got[129];
        frame_sha512(z, sizeof(z), d);
        hexdump(d, 64, got);
        CHECK(strcmp(got,
              "ab942f526272e456ed68a979f50202905ca903a141ed98443567b11ef0bf25a5"
              "52d639051a01be58558122c58e3de07d749ee59ded36acf0c55cd91924d6ba11") == 0);
        printf("  sha512(128 zeros) %s\n",
               strcmp(got,
              "ab942f526272e456ed68a979f50202905ca903a141ed98443567b11ef0bf25a5"
              "52d639051a01be58558122c58e3de07d749ee59ded36acf0c55cd91924d6ba11") == 0 ? "ok" : got);
    }

    test_ed25519_vectors();
    test_ed25519_rejects();
    test_ed25519_streaming();

    printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
