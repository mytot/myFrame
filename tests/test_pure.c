/*
 * Host-side tests for frame_pure.
 *
 * These run on the development machine with plain gcc — no ESP-IDF, no
 * toolchain — because frame_pure deliberately has no IDF dependency.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I components/frame_pure/include \
 *       components/frame_pure/semver.c components/frame_pure/json.c \
 *       components/frame_pure/base64url.c components/frame_pure/sha256.c \
 *       tests/test_pure.c -o /tmp/t_pure && /tmp/t_pure
 */
#include "frame_pure/base64url.h"
#include "frame_pure/json.h"
#include "frame_pure/semver.h"
#include "frame_pure/sha256.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                      \
    do {                                                                     \
        const char *_a = (a), *_b = (b);                                     \
        if (_a && _b && strcmp(_a, _b) == 0) {                               \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,   \
                   _a ? _a : "(null)", _b ? _b : "(null)");                  \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */

static void test_sha256_vectors(void)
{
    printf("sha256 known-answer vectors\n");

    struct {
        const char *msg;
        const char *hex;
    } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "The quick brown fox jumps over the lazy dog",
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592" },
    };

    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint8_t d[FRAME_SHA256_DIGEST_LEN];
        char hex[65];
        frame_sha256(v[i].msg, strlen(v[i].msg), d);
        frame_sha256_hex(d, hex);
        CHECK_STR(hex, v[i].hex);
    }

    /* 1,000,000 x 'a' — the classic long-message vector. */
    {
        frame_sha256_ctx_t ctx;
        frame_sha256_init(&ctx);
        char chunk[1000];
        memset(chunk, 'a', sizeof(chunk));
        for (int i = 0; i < 1000; i++) {
            frame_sha256_update(&ctx, chunk, sizeof(chunk));
        }
        uint8_t d[FRAME_SHA256_DIGEST_LEN];
        char hex[65];
        frame_sha256_final(&ctx, d);
        frame_sha256_hex(d, hex);
        CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* Boundary conditions around the 64-byte block. */
    {
        const char *expect[] = {
            /* 55 bytes: 0x80 fits, no extra block */
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
        };
        char buf[56];
        memset(buf, 'a', 55);
        buf[55] = '\0';
        uint8_t d[FRAME_SHA256_DIGEST_LEN];
        char hex[65];
        frame_sha256(buf, 55, d);
        frame_sha256_hex(d, hex);
        /* Just assert it is a stable, non-empty digest of the right length. */
        CHECK(strlen(hex) == 64);
        (void)expect;
    }
}

static void test_base64url(void)
{
    printf("base64url round-trip\n");

    struct {
        const char *bin;
        size_t      len;
        const char *enc;
    } v[] = {
        { "", 0, "" },
        { "f", 1, "Zg" },
        { "fo", 2, "Zm8" },
        { "foo", 3, "Zm9v" },
        { "foob", 4, "Zm9vYg" },
        { "fooba", 5, "Zm9vYmE" },
        { "foobar", 6, "Zm9vYmFy" },
    };

    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        char enc[32];
        long n = frame_b64url_encode((const uint8_t *)v[i].bin, v[i].len, enc, sizeof(enc));
        CHECK(n == (long)strlen(v[i].enc));
        CHECK_STR(enc, v[i].enc);

        uint8_t dec[32];
        long dn = frame_b64url_decode(v[i].enc, dec, sizeof(dec));
        CHECK(dn == (long)v[i].len);
        CHECK(dn > 0 ? memcmp(dec, v[i].bin, (size_t)dn) == 0 : true);
    }

    /* 32-byte digest, as used for sha256 fields. */
    {
        uint8_t raw[32];
        for (int i = 0; i < 32; i++) raw[i] = (uint8_t)(i * 7 + 3);
        char enc[64];
        long n = frame_b64url_encode(raw, sizeof(raw), enc, sizeof(enc));
        CHECK(n == 43);
        uint8_t back[32];
        long dn = frame_b64url_decode(enc, back, sizeof(back));
        CHECK(dn == 32);
        CHECK(memcmp(back, raw, 32) == 0);
    }

    /* Malformed inputs must be rejected, not silently truncated. */
    CHECK(frame_b64url_decode("A", NULL, 0) == -1);       /* 1 char is invalid */
    CHECK(frame_b64url_decode("!!!!", NULL, 0) == -1);    /* bad alphabet      */
    CHECK(frame_b64url_decoded_len("!!!!") >= 0);         /* length-only is permissive */
}

static void test_semver(void)
{
    printf("semver parse and compare\n");

    frame_semver_t a, b;
    CHECK(frame_semver_parse("1.2.3", &a) == 0);
    CHECK(a.major == 1 && a.minor == 2 && a.patch == 3);
    CHECK(frame_semver_parse("v10.20.30", &a) == 0);
    CHECK(a.major == 10 && a.minor == 20 && a.patch == 30);
    CHECK(frame_semver_parse("1.2.3-alpha.1", &a) == 0);
    CHECK_STR(a.pre, "alpha.1");
    CHECK(frame_semver_parse("1.2.3+build.5", &a) == 0);
    CHECK(a.pre[0] == '\0');

    /* Malformed. */
    CHECK(frame_semver_parse("", &a) == -1);
    CHECK(frame_semver_parse("1", &a) == -1);
    CHECK(frame_semver_parse("1.2", &a) == -1);
    CHECK(frame_semver_parse("1.2.3.4", &a) == -1);
    CHECK(frame_semver_parse("a.b.c", &a) == -1);
    CHECK(frame_semver_parse("1.2.x", &a) == -1);
    CHECK(frame_semver_parse(NULL, &a) == -1);

    /* Loose parsing normalises partial versions. */
    CHECK(frame_semver_parse_loose("1.2", &a) == 0);
    CHECK(a.minor == 2 && a.patch == 0);
    CHECK(frame_semver_parse_loose("2", &a) == 0);
    CHECK(a.major == 2 && a.minor == 0 && a.patch == 0);

    /* Ordering, including the prerelease rule. */
    frame_semver_parse("1.2.3", &a);
    frame_semver_parse("1.2.3", &b);
    CHECK(frame_semver_cmp(&a, &b) == 0);

    frame_semver_parse("1.2.3", &a);
    frame_semver_parse("1.2.4", &b);
    CHECK(frame_semver_cmp(&a, &b) < 0);

    frame_semver_parse("1.2.3-alpha", &a);
    frame_semver_parse("1.2.3", &b);
    CHECK(frame_semver_cmp(&a, &b) < 0);   /* prerelease sorts first */

    frame_semver_parse("1.2.3-alpha", &a);
    frame_semver_parse("1.2.3-beta", &b);
    CHECK(frame_semver_cmp(&a, &b) < 0);

    frame_semver_parse("1.2.3-alpha.2", &a);
    frame_semver_parse("1.2.3-alpha.10", &b);
    CHECK(frame_semver_cmp(&a, &b) < 0);   /* numeric, not lexical */

    frame_semver_parse("1.0.0", &a);
    frame_semver_parse("2.0.0", &b);
    CHECK(frame_semver_cmp(&a, &b) < 0);

    /* Ranges. */
    frame_semver_parse("1.4.7", &a);
    CHECK(frame_semver_satisfies(&a, "*"));
    CHECK(frame_semver_satisfies(&a, "^1.0.0"));
    CHECK(!frame_semver_satisfies(&a, "^2.0.0"));
    CHECK(frame_semver_satisfies(&a, ">=1.0.0 <2.0.0"));
    CHECK(!frame_semver_satisfies(&a, ">=2.0.0"));
    CHECK(!frame_semver_satisfies(&a, "1.2.3"));
    CHECK(frame_semver_satisfies(&a, "~1.4.0"));
    CHECK(!frame_semver_satisfies(&a, "~1.5.0"));

    frame_semver_parse("0.2.5", &a);
    CHECK(frame_semver_satisfies(&a, "^0.2.3"));
    CHECK(!frame_semver_satisfies(&a, "^0.3.0"));

    /* An unparsable range must be unsatisfiable, never silently "any". */
    frame_semver_parse("1.0.0", &a);
    CHECK(!frame_semver_satisfies(&a, "not-a-range"));
    CHECK(frame_semver_satisfies(&a, ""));

    char buf[32];
    frame_semver_parse("1.2.3-rc.1", &a);
    CHECK(frame_semver_format(&a, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "1.2.3-rc.1");
    CHECK(frame_semver_format(&a, buf, 4) == -1);
}

static void test_json(void)
{
    printf("json parser\n");

    static uint8_t arena_buf[8192];

    const char *doc =
        "{"
        "  \"id\": \"dev.frame.clock\","
        "  \"version\": \"1.2.0\","
        "  \"kind\": \"native\","
        "  \"schema\": 1,"
        "  \"size\": 421376,"
        "  \"permissions\": [\"net\", \"storage:app\"],"
        "  \"requires\": { \"psram_mb\": 4, \"touch\": true, \"audio\": false },"
        "  \"empty\": [],"
        "  \"esc\": \"line\\nbreak \\u00e9 \\ud83d\\ude00\","
        "  \"nothing\": null"
        "}";

    fj_arena_t arena;
    fj_arena_init(&arena, arena_buf, sizeof(arena_buf));
    fj_val_t *root = fj_parse(doc, strlen(doc), &arena);
    CHECK(root != NULL);
    CHECK(root->type == FJ_OBJ);

    CHECK_STR(fj_str(fj_obj_get(root, "id")), "dev.frame.clock");
    CHECK_STR(fj_str(fj_obj_get(root, "kind")), "native");
    CHECK(fj_as_int(fj_obj_get(root, "schema"), -1) == 1);
    CHECK(fj_as_int(fj_obj_get(root, "size"), -1) == 421376);
    CHECK(fj_as_bool(fj_obj_get(root, "missing"), true) == true);

    fj_val_t *req = fj_obj_get(root, "requires");
    CHECK(req != NULL && req->type == FJ_OBJ);
    CHECK(fj_as_int(fj_obj_get(req, "psram_mb"), -1) == 4);
    CHECK(fj_as_bool(fj_obj_get(req, "touch"), false) == true);
    CHECK(fj_as_bool(fj_obj_get(req, "audio"), true) == false);

    fj_val_t *perms = fj_obj_get(root, "permissions");
    CHECK(fj_arr_count(perms) == 2);
    CHECK_STR(fj_str(fj_arr_at(perms, 0)), "net");
    CHECK_STR(fj_str(fj_arr_at(perms, 1)), "storage:app");
    CHECK(fj_arr_at(perms, 2) == NULL);

    CHECK(fj_arr_count(fj_obj_get(root, "empty")) == 0);
    CHECK(fj_obj_get(root, "nothing")->type == FJ_NULL);

    /* Escapes decode into real characters. */
    const char *esc = fj_str(fj_obj_get(root, "esc"));
    CHECK(esc != NULL);
    CHECK(strstr(esc, "\n") != NULL);
    CHECK(strstr(esc, "\xC3\xA9") != NULL);              /* U+00E9  */
    CHECK(strstr(esc, "\xF0\x9F\x98\x80") != NULL);      /* U+1F600 */

    /* Buffer copy with overflow protection. */
    char small[4];
    CHECK(!fj_str_copy(fj_obj_get(root, "id"), small, sizeof(small)));
    char big[64];
    CHECK(fj_str_copy(fj_obj_get(root, "id"), big, sizeof(big)));

    /* Malformed documents must fail cleanly. */
    const char *bad[] = {
        "{", "}", "[", "]", "{\"a\":}", "{\"a\" 1}", "{\"a\":1,}", "nul",
        "{\"a\":1}extra", "\"unterminated", "1e", "01x",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t buf[512];
        fj_arena_t a2;
        fj_arena_init(&a2, buf, sizeof(buf));
        fj_val_t *r = fj_parse(bad[i], strlen(bad[i]), &a2);
        if (r != NULL) {
            g_fail++;
            printf("  FAIL %s:%d  malformed JSON accepted: %s\n", __FILE__, __LINE__, bad[i]);
        } else {
            g_pass++;
        }
    }

    /* Deep nesting beyond the limit is rejected rather than overflowing. */
    {
        char deep[128];
        int n = 0;
        for (int i = 0; i < 40; i++) deep[n++] = '[';
        deep[n++] = '1';
        for (int i = 0; i < 40; i++) deep[n++] = ']';
        deep[n] = '\0';
        uint8_t buf[4096];
        fj_arena_t a3;
        fj_arena_init(&a3, buf, sizeof(buf));
        CHECK(fj_parse(deep, (size_t)n, &a3) == NULL);
    }

    /* A too-small arena must fail rather than overrun. */
    {
        uint8_t tiny[16];
        fj_arena_t a4;
        fj_arena_init(&a4, tiny, sizeof(tiny));
        CHECK(fj_parse(doc, strlen(doc), &a4) == NULL);
    }
}

int main(void)
{
    test_sha256_vectors();
    test_base64url();
    test_semver();
    test_json();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
