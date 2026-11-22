/*
 * verify_pkg.c — host twin of frame_store/install.c's verification path.
 *
 * tools/pack.py --device-code compiles this against components/frame_pure with
 * the host compiler and runs it on a package, so the exact C that verifies on
 * the device can be exercised on a laptop (and in CI) before flashing.
 *
 *   verify_pkg <package.fpack> <publisher.pub>
 *
 * Exits 0 when the package would be accepted by a device with that key.
 */
#include "frame_pure/canonical.h"
#include "frame_pure/ed25519.h"
#include "frame_pure/manifest.h"
#include "frame_pure/semver.h"
#include "frame_pure/sha256.h"
#include "frame_pure/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDK_VERSION "1.0.0"
#define MANIFEST_MAX 8192u

typedef struct {
    frame_ed25519_verify_ctx_t ed;
    frame_sha256_ctx_t         sha;
    uint8_t                    pubkey[32];
    const char                *stage;
} verify_t;

static int payload_sink(void *ctx, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    verify_t *v = (verify_t *)ctx;
    frame_ed25519_verify_update(&v->ed, data, len);
    frame_sha256_update(&v->sha, data, len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: verify_pkg <package.fpack> <publisher.pub>\n");
        return 2;
    }

    FILE *kf = fopen(argv[2], "rb");
    uint8_t pub[32];
    if (kf == NULL || fread(pub, 1, 32, kf) != 32) {
        fprintf(stderr, "bad public key file\n");
        return 2;
    }
    fclose(kf);

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    frame_zip_t *z = frame_zip_open(f);
    if (z == NULL) {
        fprintf(stderr, "not a zip\n");
        return 1;
    }
    const frame_zip_entry_t *me = frame_zip_find(z, "manifest.json");
    const frame_zip_entry_t *pe = frame_zip_find(z, "payload.bin");
    const frame_zip_entry_t *se = frame_zip_find(z, "signature.bin");
    if (me == NULL || pe == NULL || se == NULL) {
        fprintf(stderr, "missing manifest/payload/signature\n");
        return 1;
    }
    if (me->method != 0u || pe->method != 0u) {
        fprintf(stderr, "members must be STORED (no deflate)\n");
        return 1;
    }

    static uint8_t mtext[MANIFEST_MAX];
    long mlen = frame_zip_read(z, me, mtext, MANIFEST_MAX - 1u);
    if (mlen < 0) {
        fprintf(stderr, "manifest unreadable\n");
        return 1;
    }
    mtext[mlen] = '\0';

    frame_manifest_t m;
    int rc = frame_manifest_parse((const char *)mtext, (size_t)mlen, &m);
    if (rc != FRAME_MANIFEST_OK) {
        fprintf(stderr, "manifest rejected (%d)\n", rc);
        return 1;
    }
    frame_device_caps_t caps = {
        .chip = "esp32s3", .psram_mb = 8, .width = 800, .height = 480,
        .touch = true, .audio = false, .sdk_version = SDK_VERSION,
    };
    rc = frame_manifest_check_requirements(&m, &caps);
    if (rc != FRAME_REQ_OK) {
        fprintf(stderr, "manifest requirements not met by a stock 8048S070 (%d)\n", rc);
        return 1;
    }
    frame_semver_t sdk;
    if (frame_semver_parse(SDK_VERSION, &sdk) != 0 || !frame_semver_satisfies(&sdk, m.sdk)) {
        fprintf(stderr, "SDK range %s does not include " SDK_VERSION "\n", m.sdk);
        return 1;
    }
    if (m.size != 0u && m.size != pe->uncomp_size) {
        fprintf(stderr, "size %u != payload %u\n", (unsigned)m.size, (unsigned)pe->uncomp_size);
        return 1;
    }

    static char canonical[MANIFEST_MAX];
    if (!frame_canonicalise((const char *)mtext, (size_t)mlen, canonical, sizeof(canonical))) {
        fprintf(stderr, "cannot canonicalise manifest\n");
        return 1;
    }

    uint8_t sig[64];
    if (frame_zip_read(z, se, sig, sizeof(sig)) != (long)sizeof(sig)) {
        fprintf(stderr, "signature must be 64 bytes\n");
        return 1;
    }

    verify_t v;
    frame_sha256_init(&v.sha);
    frame_ed25519_verify_begin(&v.ed, sig, pub);
    frame_ed25519_verify_update(&v.ed, canonical, strlen(canonical));
    long n = frame_zip_stream(z, pe, payload_sink, &v);
    frame_zip_close(z);
    fclose(f);
    if (n < 0) {
        fprintf(stderr, "payload stream failed (%ld)\n", n);
        return 1;
    }

    uint8_t digest[32];
    char hex[65];
    frame_sha256_final(&v.sha, digest);
    frame_sha256_hex(digest, hex);
    if (m.sha256[0] != '\0' && strcmp(hex, m.sha256) != 0) {
        fprintf(stderr, "payload digest mismatch: %s vs %s\n", hex, m.sha256);
        return 1;
    }
    if (frame_ed25519_verify_end(&v.ed) != 0) {
        fprintf(stderr, "SIGNATURE INVALID\n");
        return 1;
    }
    printf("verified %s %s (%s, %u B payload) with the device's verifier\n",
           m.id, m.version, frame_manifest_kind_str(m.kind), (unsigned)n);
    return 0;
}
