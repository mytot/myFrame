/*
 * Host-side tests for the package/manifest/slot logic in frame_pure.
 * See tests/run.sh for the exact build line.
 */
#include "frame_pure/canonical.h"
#include "frame_pure/manifest.h"
#include "frame_pure/sha256.h"
#include "frame_pure/slot_table.h"

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

static void test_canonical(void)
{
    printf("canonical json\n");

    /* Key order and whitespace must not affect the signature. */
    const char *a = "{\"b\":1,\"a\":2}";
    const char *b = "{ \"a\" : 2 , \"b\" : 1 }";
    char ca[128], cb[128];

    CHECK(frame_canonicalise(a, strlen(a), ca, sizeof(ca)));
    CHECK(frame_canonicalise(b, strlen(b), cb, sizeof(cb)));
    CHECK_STR(ca, cb);
    CHECK_STR(ca, "{\"a\":2,\"b\":1}");

    /* Integral floats normalise to integers so 1.0 and 1 hash identically. */
    const char *f1 = "{\"n\":1}";
    const char *f2 = "{\"n\":1.0}";
    CHECK(frame_canonicalise(f1, strlen(f1), ca, sizeof(ca)));
    CHECK(frame_canonicalise(f2, strlen(f2), cb, sizeof(cb)));
    CHECK_STR(ca, cb);

    /* Nested objects are sorted at every level. */
    const char *nested = "{\"z\":{\"y\":1,\"x\":2},\"a\":[3,1,2]}";
    CHECK(frame_canonicalise(nested, strlen(nested), ca, sizeof(ca)));
    CHECK_STR(ca, "{\"a\":[3,1,2],\"z\":{\"x\":2,\"y\":1}}");

    /* Array order is preserved — it is semantically significant. */
    CHECK(strstr(ca, "[3,1,2]") != NULL);

    /* Strings are escaped minimally and correctly. */
    const char *esc = "{\"s\":\"a\\\"b\\\\c\\nd\"}";
    CHECK(frame_canonicalise(esc, strlen(esc), ca, sizeof(ca)));
    CHECK_STR(ca, "{\"s\":\"a\\\"b\\\\c\\nd\"}");

    /* Empty containers. */
    CHECK(frame_canonicalise("{}", 2, ca, sizeof(ca)));
    CHECK_STR(ca, "{}");
    CHECK(frame_canonicalise("[]", 2, ca, sizeof(ca)));
    CHECK_STR(ca, "[]");

    /* Malformed input and overflow are reported, not truncated. */
    CHECK(!frame_canonicalise("{", 1, ca, sizeof(ca)));
    char tiny[4];
    CHECK(!frame_canonicalise("{\"a\":1}", 7, tiny, sizeof(tiny)));
}

/* ------------------------------------------------------------------ */

static const char *GOOD_MANIFEST =
    "{"
    "  \"schema\": 1,"
    "  \"id\": \"dev.frame.clock\","
    "  \"name\": \"Clock\","
    "  \"version\": \"1.2.0\","
    "  \"kind\": \"native\","
    "  \"sdk\": \"^1.0.0\","
    "  \"requires\": {"
    "    \"chip\": [\"esp32s3\"],"
    "    \"psram_mb\": 4,"
    "    \"display\": { \"min_width\": 480, \"min_height\": 320, \"touch\": true },"
    "    \"audio\": false"
    "  },"
    "  \"permissions\": [\"net\", \"storage:app\"],"
    "  \"entry\": \"payload.bin\","
    "  \"size\": 421376,"
    "  \"sha256\": \"9f2c1a3b4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8\","
    "  \"description\": \"A clock.\","
    "  \"author\": \"Frame Labs\""
    "}";

static void test_manifest(void)
{
    printf("manifest parse\n");

    frame_manifest_t m;
    int rc = frame_manifest_parse(GOOD_MANIFEST, strlen(GOOD_MANIFEST), &m);
    CHECK(rc == FRAME_MANIFEST_OK);
    CHECK_STR(m.id, "dev.frame.clock");
    CHECK_STR(m.name, "Clock");
    CHECK_STR(m.version, "1.2.0");
    CHECK(m.kind == FRAME_MANIFEST_NATIVE);
    CHECK_STR(m.sdk, "^1.0.0");
    CHECK(m.size == 421376u);
    CHECK(m.requires.psram_mb == 4);
    CHECK(m.requires.min_width == 480);
    CHECK(m.requires.touch == true);
    CHECK(m.requires.audio == false);
    CHECK(m.requires.chip_count == 1);
    CHECK_STR(m.requires.chips[0], "esp32s3");
    CHECK(m.permission_count == 2);
    CHECK(frame_manifest_has_permission(&m, "net"));
    CHECK(frame_manifest_has_permission(&m, "storage:app"));
    CHECK(!frame_manifest_has_permission(&m, "audio"));
    CHECK(m.render.cls == FRAME_RENDER_LIGHT);  /* default when unspecified */

    /* --- requirement matching --------------------------------------- */
    frame_device_caps_t caps = {
        .chip = "esp32s3", .psram_mb = 8, .width = 800, .height = 480,
        .touch = true, .audio = true, .sdk_version = "1.4.2",
    };
    CHECK(frame_manifest_check_requirements(&m, &caps) == FRAME_REQ_OK);

    frame_device_caps_t bad = caps;
    bad.chip = "esp32c3";
    CHECK(frame_manifest_check_requirements(&m, &bad) == FRAME_REQ_ERR_CHIP);

    bad = caps; bad.psram_mb = 2;
    CHECK(frame_manifest_check_requirements(&m, &bad) == FRAME_REQ_ERR_PSRAM);

    bad = caps; bad.width = 320;
    CHECK(frame_manifest_check_requirements(&m, &bad) == FRAME_REQ_ERR_DISPLAY);

    bad = caps; bad.touch = false;
    CHECK(frame_manifest_check_requirements(&m, &bad) == FRAME_REQ_ERR_TOUCH);

    bad = caps; bad.sdk_version = "0.9.0";
    CHECK(frame_manifest_check_requirements(&m, &bad) == FRAME_REQ_ERR_SDK);

    /* --- rejection cases -------------------------------------------- */
    frame_manifest_t t;

    /* Wrong schema version is refused outright. */
    const char *s2 = "{\"schema\":2,\"id\":\"a\",\"name\":\"a\",\"version\":\"1.0.0\","
                     "\"kind\":\"native\",\"sdk\":\"^1.0.0\",\"entry\":\"p\",\"sha256\":\""
                     "0000000000000000000000000000000000000000000000000000000000000000\"}";
    CHECK(frame_manifest_parse(s2, strlen(s2), &t) == FRAME_MANIFEST_ERR_SCHEMA);

    /* Missing a required field. */
    const char *s3 = "{\"schema\":1,\"id\":\"a\",\"name\":\"a\",\"version\":\"1.0.0\","
                     "\"kind\":\"native\",\"sdk\":\"^1.0.0\",\"entry\":\"p\"}";
    CHECK(frame_manifest_parse(s3, strlen(s3), &t) == FRAME_MANIFEST_ERR_FIELD);

    /* Malformed version. */
    const char *s4 = "{\"schema\":1,\"id\":\"a\",\"name\":\"a\",\"version\":\"not.semver\","
                     "\"kind\":\"native\",\"sdk\":\"^1.0.0\",\"entry\":\"p\","
                     "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    CHECK(frame_manifest_parse(s4, strlen(s4), &t) == FRAME_MANIFEST_ERR_FIELD);

    /* Digest of the wrong length. */
    const char *s5 = "{\"schema\":1,\"id\":\"a\",\"name\":\"a\",\"version\":\"1.0.0\","
                     "\"kind\":\"native\",\"sdk\":\"^1.0.0\",\"entry\":\"p\",\"sha256\":\"abcd\"}";
    CHECK(frame_manifest_parse(s5, strlen(s5), &t) == FRAME_MANIFEST_ERR_FIELD);

    /* Unknown kind. */
    const char *s6 = "{\"schema\":1,\"id\":\"a\",\"name\":\"a\",\"version\":\"1.0.0\","
                     "\"kind\":\"exe\",\"sdk\":\"^1.0.0\",\"entry\":\"p\","
                     "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    CHECK(frame_manifest_parse(s6, strlen(s6), &t) == FRAME_MANIFEST_ERR_UNKNOWN);

    /* Malformed JSON. */
    CHECK(frame_manifest_parse("{", 1, &t) == FRAME_MANIFEST_ERR_PARSE);

    /* Render class round-trip. */
    frame_render_class_t c;
    CHECK(frame_render_class_from_str("full", &c) == FRAME_MANIFEST_OK && c == FRAME_RENDER_FULL);
    CHECK(frame_render_class_from_str("nope", &c) != FRAME_MANIFEST_OK);
    CHECK_STR(frame_render_class_str(FRAME_RENDER_MEDIUM), "medium");
}

/* ------------------------------------------------------------------ */

static bool ref_clock(const char *id, void *ctx)
{
    (void)ctx;
    return strcmp(id, "dev.frame.clock") == 0;
}

static void test_slot_table(void)
{
    printf("slot table\n");

    frame_slot_table_t t;
    frame_slot_table_init(&t, 4);
    CHECK(t.count == 4);

    uint8_t fr, us;
    frame_slot_table_counts(&t, &fr, &us);
    CHECK(fr == 4 && us == 0);

    /* Reserve and commit. */
    int s0 = frame_slot_table_reserve(&t, 1000);
    CHECK(s0 == 0);
    CHECK(t.slots[0].state == FRAME_SLOT_RESERVED);
    frame_slot_table_counts(&t, &fr, &us);
    CHECK(fr == 3 && us == 1);

    CHECK(frame_slot_table_commit(&t, 0, "dev.frame.clock", "1.0.0") == 0);
    CHECK(t.slots[0].state == FRAME_SLOT_VALID);
    CHECK_STR(t.slots[0].app_id, "dev.frame.clock");
    CHECK_STR(t.slots[0].version, "1.0.0");

    /* Committing an unreserved slot must fail. */
    CHECK(frame_slot_table_commit(&t, 1, "dev.frame.weather", "1.0.0") == -1);
    CHECK(t.slots[1].state == FRAME_SLOT_FREE);

    /* Fill the rest. */
    int s1 = frame_slot_table_reserve(&t, 1100);
    int s2 = frame_slot_table_reserve(&t, 1200);
    CHECK(s1 == 1 && s2 == 2);
    CHECK(frame_slot_table_commit(&t, 1, "dev.frame.weather", "2.0.0") == 0);
    CHECK(frame_slot_table_commit(&t, 2, "dev.frame.notes", "0.1.0") == 0);

    /* Fill the last slot, then the table is genuinely full. */
    int s3f = frame_slot_table_reserve(&t, 1300);
    CHECK(s3f == 3);
    CHECK(frame_slot_table_commit(&t, 3, "dev.frame.extra", "1.0.0") == 0);
    CHECK(frame_slot_table_reserve(&t, 1400) == -1);

    /* Find. */
    CHECK(frame_slot_table_find(&t, "dev.frame.weather") == 1);
    CHECK(frame_slot_table_find(&t, "nope") == -1);

    /* LRU eviction protects the named app. */
    frame_slot_table_touch(&t, 0, 5000);   /* clock  — most recent */
    frame_slot_table_touch(&t, 1, 2000);   /* weather — oldest     */
    frame_slot_table_touch(&t, 2, 3000);   /* notes                */
    frame_slot_table_touch(&t, 3, 4000);   /* extra                */
    CHECK(frame_slot_table_evict_lru(&t, NULL) == 1);                    /* weather */
    CHECK(frame_slot_table_evict_lru(&t, "dev.frame.weather") == 2);     /* notes   */
    CHECK(frame_slot_table_evict_lru(&t, "dev.frame.weather") != 1);

    /* Release only applies to RESERVED. */
    frame_slot_table_release(&t, 0);
    CHECK(t.slots[0].state == FRAME_SLOT_VALID);   /* committed: untouched */

    /* Free one slot, reserve it again, and confirm release works there. */
    memset(&t.slots[3], 0, sizeof(t.slots[3]));
    t.slots[3].state = FRAME_SLOT_FREE;
    int sr = frame_slot_table_reserve(&t, 1400);
    CHECK(sr == 3);
    frame_slot_table_release(&t, 3);
    CHECK(t.slots[3].state == FRAME_SLOT_FREE);

    /* --- sweep: expired reservations are reclaimed -------------------- */
    frame_slot_table_init(&t, 3);
    int r0 = frame_slot_table_reserve(&t, 1000);
    int r1 = frame_slot_table_reserve(&t, 2000);
    CHECK(r0 == 0 && r1 == 1);
    frame_slot_table_commit(&t, 1, "dev.frame.a", "1.0.0");

    /* r0 is 5000ms old with a 10 000ms expiry: still reserved. */
    frame_slot_table_sweep(&t, 6000, 10000, NULL, NULL);
    CHECK(t.slots[0].state == FRAME_SLOT_RESERVED);

    /* Past expiry: reclaimed. */
    frame_slot_table_sweep(&t, 11000, 10000, NULL, NULL);
    CHECK(t.slots[0].state == FRAME_SLOT_FREE);
    CHECK(t.slots[1].state == FRAME_SLOT_VALID);

    /* --- sweep: unreferenced VALID slots become STALE ----------------- */
    frame_slot_table_init(&t, 3);
    frame_slot_table_reserve(&t, 0);
    frame_slot_table_commit(&t, 0, "dev.frame.clock", "1.0.0");
    frame_slot_table_reserve(&t, 0);
    frame_slot_table_commit(&t, 1, "dev.frame.orphan", "1.0.0");
    frame_slot_table_sweep(&t, 500, 10000, ref_clock, NULL);
    CHECK(t.slots[0].state == FRAME_SLOT_VALID);
    CHECK(t.slots[1].state == FRAME_SLOT_STALE);

    /* With no callback, nothing is demoted. */
    frame_slot_table_init(&t, 3);
    frame_slot_table_reserve(&t, 0);
    frame_slot_table_commit(&t, 0, "dev.frame.orphan", "1.0.0");
    frame_slot_table_sweep(&t, 500, 10000, NULL, NULL);
    CHECK(t.slots[0].state == FRAME_SLOT_VALID);

    /* --- 32-bit clock wrap-around ------------------------------------ */
    frame_slot_table_init(&t, 2);
    frame_slot_table_reserve(&t, 0xFFFFF000u);
    /* 0x1000 - 0xFFFFF000 wraps to 0x2000 = 8192 ms elapsed. */
    frame_slot_table_sweep(&t, 0x00001000u, 8000, NULL, NULL);
    CHECK(t.slots[0].state == FRAME_SLOT_FREE);
}

/* ------------------------------------------------------------------ */

static void test_digest_pipeline(void)
{
    printf("package digest pipeline\n");

    /* The device hashes canonical(manifest) || payload. Reproduce the same
     * construction here so the two sides cannot drift apart. */
    char canon[2048];
    CHECK(frame_canonicalise(GOOD_MANIFEST, strlen(GOOD_MANIFEST), canon, sizeof(canon)));

    const char *payload = "This is the payload bytes.";
    frame_sha256_ctx_t ctx;
    frame_sha256_init(&ctx);
    frame_sha256_update(&ctx, canon, strlen(canon));
    frame_sha256_update(&ctx, payload, strlen(payload));
    uint8_t d[FRAME_SHA256_DIGEST_LEN];
    frame_sha256_final(&ctx, d);

    /* Changing the payload must change the digest. */
    frame_sha256_ctx_t c2;
    frame_sha256_init(&c2);
    frame_sha256_update(&c2, canon, strlen(canon));
    frame_sha256_update(&c2, payload, strlen(payload) - 1u);
    uint8_t d2[FRAME_SHA256_DIGEST_LEN];
    frame_sha256_final(&c2, d2);
    CHECK(memcmp(d, d2, FRAME_SHA256_DIGEST_LEN) != 0);

    /* Reordering keys must NOT change the digest. */
    const char *reordered =
        "{\"name\":\"Clock\",\"id\":\"dev.frame.clock\",\"schema\":1,\"kind\":\"native\","
        "\"version\":\"1.2.0\",\"sdk\":\"^1.0.0\",\"entry\":\"payload.bin\","
        "\"size\":421376,\"author\":\"Frame Labs\",\"description\":\"A clock.\","
        "\"permissions\":[\"net\",\"storage:app\"],"
        "\"requires\":{\"audio\":false,\"psram_mb\":4,\"chip\":[\"esp32s3\"],"
        "\"display\":{\"touch\":true,\"min_height\":320,\"min_width\":480}},"
        "\"sha256\":\"9f2c1a3b4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8\"}";
    char canon2[2048];
    CHECK(frame_canonicalise(reordered, strlen(reordered), canon2, sizeof(canon2)));
    CHECK_STR(canon, canon2);
}

int main(void)
{
    test_canonical();
    test_manifest();
    test_slot_table();
    test_digest_pipeline();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
