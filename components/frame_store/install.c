/*
 * install.c — turn a verified .fpack into an installed app.
 *
 * Order of operations is the whole design (docs/APP_STORE.md §6):
 *
 *   1. download to a staging file          (nothing installed yet)
 *   2. parse + canonicalise the manifest   (no flash writes)
 *   3. check requirements + SDK range      (no flash writes)
 *   4. stream payload: verify signature, hash, write to a RESERVED slot or a
 *      .tmp file                           (nothing bootable yet)
 *   5. commit slot / rename .tmp           (now it exists)
 *   6. write the registry entry            (now it is installed)
 *
 * A power cut between 4 and 5 leaves a reclaimable slot or a .tmp file; a cut
 * between 5 and 6 leaves an unreferenced slot that the next reconcile demotes.
 * Nothing leaves a registry entry pointing at nothing.
 */
#include "frame_store/store.h"
#include "store_internal.h"

#include "frame_core/app.h"
#include "frame_core/event.h"
#include "frame_log/log.h"
#include "frame_net/net.h"
#include "frame_ota/ota.h"
#include "frame_pure/canonical.h"
#include "frame_pure/ed25519.h"
#include "frame_pure/manifest.h"
#include "frame_pure/semver.h"
#include "frame_pure/sha256.h"
#include "frame_pure/zip.h"
#include "frame_storage/storage.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "install";

/* ------------------------------------------------------------------ */
/* Package handle                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE                 *f;
    frame_zip_t          *zip;
    frame_manifest_t      manifest;
    char                  canonical[MANIFEST_MAX_BYTES];
    size_t                canon_len;
    const frame_zip_entry_t *payload;
    uint8_t               sig[FRAME_ED25519_SIGNATURE_LEN];
} pkg_t;

static void pkg_close(pkg_t *p)
{
    if (p->zip != NULL) {
        frame_zip_close(p->zip);
        p->zip = NULL;
    }
    if (p->f != NULL) {
        fclose(p->f);
        p->f = NULL;
    }
}

static esp_err_t pkg_open(pkg_t *p, const char *path)
{
    memset(p, 0, sizeof(*p));

    p->f = fopen(path, "rb");
    if (p->f == NULL) {
        store_report_error("package not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    p->zip = frame_zip_open(p->f);
    if (p->zip == NULL) {
        pkg_close(p);
        store_report_error("package is not a valid zip");
        return ESP_ERR_INVALID_CRC;
    }

    const frame_zip_entry_t *m = frame_zip_find(p->zip, "manifest.json");
    if (m == NULL) {
        pkg_close(p);
        store_report_error("package has no manifest.json");
        return ESP_ERR_NOT_FOUND;
    }
    /* Heap, not stack: the manifest allowance is 8 KB and callers run on
     * tasks with modest stacks. */
    uint8_t *mtext = malloc(MANIFEST_MAX_BYTES);
    if (mtext == NULL) {
        pkg_close(p);
        store_report_error("out of memory reading manifest");
        return ESP_ERR_NO_MEM;
    }
    const long mlen = frame_zip_read(p->zip, m, mtext, MANIFEST_MAX_BYTES - 1u);
    if (mlen < 0) {
        free(mtext);
        pkg_close(p);
        store_report_error("manifest unreadable (%ld)", mlen);
        return ESP_ERR_INVALID_SIZE;
    }
    mtext[mlen] = '\0';

    int rc = frame_manifest_parse((const char *)mtext, (size_t)mlen, &p->manifest);
    if (rc != FRAME_MANIFEST_OK) {
        free(mtext);
        pkg_close(p);
        store_report_error("manifest rejected (%d)", rc);
        return ESP_ERR_INVALID_ARG;
    }

    frame_device_caps_t caps;
    frame_store_device_caps(&caps);
    rc = frame_manifest_check_requirements(&p->manifest, &caps);
    if (rc != FRAME_REQ_OK) {
        free(mtext);
        pkg_close(p);
        store_report_error("incompatible with this device (%d)", rc);
        return ESP_ERR_NOT_SUPPORTED;
    }

    frame_semver_t sdk;
    if (frame_semver_parse(FRAME_SDK_VERSION, &sdk) != 0 ||
        !frame_semver_satisfies(&sdk, p->manifest.sdk)) {
        free(mtext);
        pkg_close(p);
        store_report_error("needs Frame SDK %s", p->manifest.sdk);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!frame_canonicalise((const char *)mtext, (size_t)mlen, p->canonical, sizeof(p->canonical))) {
        free(mtext);
        pkg_close(p);
        store_report_error("manifest cannot be canonicalised");
        return ESP_ERR_INVALID_SIZE;
    }
    p->canon_len = strlen(p->canonical);

    /* `entry` names the payload member; "payload.bin" is the convention. */
    const char *entry = (p->manifest.entry[0] != '\0') ? p->manifest.entry : "payload.bin";
    p->payload = frame_zip_find(p->zip, entry);
    if (p->payload == NULL || p->payload->method != 0u) {
        free(mtext);
        pkg_close(p);
        store_report_error("package payload missing or compressed");
        return ESP_ERR_NOT_FOUND;
    }
    if (p->manifest.size != 0u && p->manifest.size != p->payload->uncomp_size) {
        free(mtext);
        pkg_close(p);
        store_report_error("payload size does not match manifest");
        return ESP_ERR_INVALID_SIZE;
    }

    const frame_zip_entry_t *s = frame_zip_find(p->zip, "signature.bin");
    if (s == NULL || frame_zip_read(p->zip, s, p->sig, sizeof(p->sig)) != (long)sizeof(p->sig)) {
        free(mtext);
        pkg_close(p);
        store_report_error("package signature missing");
        return ESP_ERR_NOT_FOUND;
    }
    free(mtext);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Streaming: verify + write in one pass                               */
/* ------------------------------------------------------------------ */

typedef struct {
    frame_ed25519_verify_ctx_t ed;
    frame_sha256_ctx_t         sha;
    frame_ota_writer_t        *ota;      /* native target  */
    FILE                      *out;      /* managed target */
    const frame_store_ops_t   *ops;
    int64_t                    total;
    int64_t                    done;
    esp_err_t                  err;
    bool                       verify_sig;
} stream_ctx_t;

static int payload_sink(void *ctx, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    stream_ctx_t *sc = (stream_ctx_t *)ctx;

    if (sc->verify_sig) {
        frame_ed25519_verify_update(&sc->ed, data, len);
    }
    frame_sha256_update(&sc->sha, data, len);

    if (sc->err == ESP_OK) {
        if (sc->ota != NULL) {
            /* 8 KB per call is the write bound: flash programming pauses
             * between chunks so the LVGL task never misses a vsync. */
            if (frame_ota_write_chunk(sc->ota, data, len) != ESP_OK) {
                sc->err = ESP_FAIL;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else if (sc->out != NULL) {
            if (fwrite(data, 1, len, sc->out) != len) {
                sc->err = ESP_FAIL;
            }
        }
    }

    sc->done += (int64_t)len;
    if (sc->ops != NULL && sc->ops->progress != NULL && sc->total > 0) {
        /* Verification+install spans 25%..90% of the overall progress. */
        int pct = 25 + (int)((sc->done * 65) / sc->total);
        sc->ops->progress(pct, sc->ota != NULL ? "Writing flash" : "Saving app", sc->ops->user);
    }
    return (sc->err == ESP_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Slot helpers                                                         */
/* ------------------------------------------------------------------ */

esp_err_t store_erase_stale_slots(unsigned max_slots)
{
    const frame_slot_table_t *t = frame_ota_table();
    unsigned erased = 0;
    for (uint8_t i = 0; i < t->count && erased < max_slots; i++) {
        if (t->slots[i].state == FRAME_SLOT_STALE) {
            ESP_LOGI(TAG, "reclaiming stale slot %u", (unsigned)i);
            esp_err_t err = frame_ota_erase_slot(i);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "erase slot %u failed: %s", (unsigned)i, esp_err_to_name(err));
                continue;
            }
            frame_ota_release_slot(i);
            erased++;
        }
    }
    return ESP_OK;
}

static int reserve_slot_for_update(const char *app_id)
{
    int slot = frame_ota_reserve_slot();
    if (slot >= 0) {
        return slot;
    }
    /* Table full: reclaim a stale slot first, then evict the least recently
     * launched app that is not the one being updated. */
    (void)store_erase_stale_slots(1u);
    slot = frame_ota_reserve_slot();
    if (slot >= 0) {
        return slot;
    }
    const int victim = frame_ota_evict_lru(app_id);
    if (victim >= 0) {
        ESP_LOGW(TAG, "slot table full: evicting slot %d", victim);
        if (frame_ota_erase_slot(victim) == ESP_OK) {
            frame_ota_release_slot(victim);
            slot = frame_ota_reserve_slot();
        }
    }
    return slot;
}

/* ------------------------------------------------------------------ */
/* Managed app files                                                    */
/* ------------------------------------------------------------------ */

static void managed_dir(const char *app_id, char *out, size_t cap)
{
    snprintf(out, cap, FRAME_STORE_APPS_DIR "/%s", app_id);
}

static esp_err_t write_manifest_copy(const char *app_id, const char *canonical)
{
    char path[FRAME_APP_PATH_MAX + 32];
    snprintf(path, sizeof(path), FRAME_STORE_APPS_DIR "/%s/manifest.json", app_id);
    return store_file_write(path, (const uint8_t *)canonical, strlen(canonical));
}

static void extract_assets(pkg_t *p, const char *app_id)
{
    static const char prefix[] = "assets/";
    char dir[FRAME_APP_PATH_MAX + 32];
    snprintf(dir, sizeof(dir), FRAME_STORE_APPS_DIR "/%s/assets", app_id);
    (void)store_mkdir(dir);

    for (size_t i = 0; i < frame_zip_count(p->zip); i++) {
        const frame_zip_entry_t *e = frame_zip_at(p->zip, i);
        if (e == NULL || e->method != 0u || strncmp(e->name, prefix, sizeof(prefix) - 1u) != 0) {
            continue;
        }
        const char *rel = e->name + sizeof(prefix) - 1u;
        if (rel[0] == '\0' || strchr(rel, '/') != NULL || strstr(rel, "..") != NULL) {
            continue;   /* flat directory only: no traversal, no nesting */
        }
        char path[FRAME_APP_PATH_MAX + 64];
        snprintf(path, sizeof(path), "%s/%s", dir, rel);
        uint8_t *buf = malloc(e->uncomp_size + 1u);
        if (buf == NULL) {
            continue;
        }
        if (frame_zip_read(p->zip, e, buf, e->uncomp_size) == (long)e->uncomp_size) {
            (void)store_file_write(path, buf, e->uncomp_size);
        }
        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Core install                                                         */
/* ------------------------------------------------------------------ */

static void publish_installed(const frame_manifest_t *m, uint8_t slot)
{
    frame_app_state_t st;
    memset(&st, 0, sizeof(st));
    store_copy(st.id, sizeof(st.id), m->id);
    store_copy(st.name, sizeof(st.name), m->name);
    store_copy(st.version, sizeof(st.version), m->version);
    st.kind     = (m->kind == FRAME_MANIFEST_NATIVE) ? FRAME_APP_KIND_NATIVE
                                                     : FRAME_APP_KIND_MANAGED;
    st.slot     = slot;
    st.builtin  = false;
    st.healthy  = true;

    /* Registry first (durable), then the in-memory manager the launcher reads. */
    esp_err_t err = frame_registry_save(&st);
    if (err == ESP_OK) {
        err = frame_app_manager_upsert(&st);
    }
    frame_app_manager_clear_available(m->id);

    frame_event_t e = { .type = FRAME_EVT_APP_INSTALLED, .arg0 = (int32_t)slot };
    store_copy(e.text, sizeof(e.text), m->id);
    (void)frame_event_post(&e);
    FRAME_LOGI(TAG, "installed %s %s (%s, slot %u)", m->id, m->version,
                   st.kind == FRAME_APP_KIND_NATIVE ? "native" : "managed", (unsigned)slot);
}

esp_err_t store_install_fpack_file(const char *path, const uint8_t pub[32],
                                   const frame_store_ops_t *ops)
{
    /* ~8 KB of canonical manifest text lives in pkg_t: heap, not stack. */
    pkg_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pkg_open(p, path);
    if (err != ESP_OK) {
        free(p);
        return err;
    }

    const bool verify_sig = !frame_store_unsigned_allowed();
    if (verify_sig && pub == NULL) {
        store_report_error("no publisher key for %s", p->manifest.id);
        pkg_close(p);
        free(p);
        return ESP_ERR_INVALID_STATE;
    }

    stream_ctx_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.ops       = ops;
    sc.total     = (int64_t)p->payload->uncomp_size;
    sc.verify_sig = verify_sig;
    frame_sha256_init(&sc.sha);
    if (verify_sig) {
        frame_ed25519_verify_begin(&sc.ed, p->sig, pub);
        frame_ed25519_verify_update(&sc.ed, p->canonical, p->canon_len);
    }

    const bool native = (p->manifest.kind == FRAME_MANIFEST_NATIVE);
    int old_slot = native ? frame_ota_find_slot(p->manifest.id) : -1;
    int slot = -1;
    char wasm_path[FRAME_APP_PATH_MAX + 32];
    char tmp_path[FRAME_APP_PATH_MAX + 40];
    tmp_path[0] = '\0';

    if (native) {
        slot = reserve_slot_for_update(p->manifest.id);
        if (slot < 0 || frame_ota_write_begin(slot, p->payload->uncomp_size, &sc.ota) != ESP_OK) {
            pkg_close(p);
            free(p);
            store_report_error("no free app slot");
            return ESP_ERR_NO_MEM;
        }
    } else {
        if (frame_store_managed_path(p->manifest.id, wasm_path, sizeof(wasm_path)) != ESP_OK ||
            !store_fs_ready()) {
            pkg_close(p);
            free(p);
            store_report_error("asset filesystem unavailable");
            return ESP_ERR_INVALID_STATE;
        }
        char dir[FRAME_APP_PATH_MAX + 32];
        managed_dir(p->manifest.id, dir, sizeof(dir));
        store_mkdir(dir);
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", wasm_path);
        sc.out = fopen(tmp_path, "wb");
        if (sc.out == NULL) {
            pkg_close(p);
            free(p);
            store_report_error("cannot write app file");
            return ESP_FAIL;
        }
    }

    store_progress(ops, 25, native ? "Writing flash" : "Saving app");
    const long written = frame_zip_stream(p->zip, p->payload, payload_sink, &sc);

    bool ok = (written >= 0) && (sc.err == ESP_OK);

    /* Digest must match what the manifest promised. */
    uint8_t digest[FRAME_SHA256_DIGEST_LEN];
    frame_sha256_final(&sc.sha, digest);
    char hex[65];
    frame_sha256_hex(digest, hex);
    if (ok && p->manifest.sha256[0] != '\0' && strncmp(hex, p->manifest.sha256, 64) != 0) {
        store_report_error("payload digest mismatch");
        ok = false;
    }
    if (ok && verify_sig && frame_ed25519_verify_end(&sc.ed) != 0) {
        store_report_error("signature invalid: %s", p->manifest.id);
        ok = false;
    }

    if (!ok) {
        pkg_close(p);
        if (native) {
            frame_ota_write_abort(sc.ota);
        } else {
            if (sc.out != NULL) {
                fclose(sc.out);
            }
            if (tmp_path[0] != '\0') {
                remove(tmp_path);
            }
        }
        free(p);
        return ESP_ERR_INVALID_CRC;
    }

    /* Assets are read from the package, so this must happen while the zip is
     * still open — and only after the signature has been verified. */
    if (!native) {
        extract_assets(p, p->manifest.id);
    }
    pkg_close(p);

    if (native) {
        err = frame_ota_write_end(sc.ota, p->manifest.id, p->manifest.version);
        if (err != ESP_OK) {
            store_report_error("image rejected: %s", esp_err_to_name(err));
            free(p);
            return err;
        }
    } else {
        fflush(sc.out);
        fclose(sc.out);
        sc.out = NULL;
        if (rename(tmp_path, wasm_path) != 0) {
            remove(tmp_path);
            store_report_error("cannot finalise app file");
            free(p);
            return ESP_FAIL;
        }
        (void)write_manifest_copy(p->manifest.id, p->canonical);
    }

    publish_installed(&p->manifest, native ? (uint8_t)slot : FRAME_APP_NO_SLOT);
    store_progress(ops, 100, "Installed");

    /* Reclaim the previous version's slot now that the new one is committed
     * and registered. This runs after the UI has been told "done". */
    if (native && old_slot >= 0 && old_slot != slot) {
        if (frame_ota_erase_slot(old_slot) == ESP_OK) {
            frame_ota_release_slot(old_slot);
        }
    }
    free(p);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Catalog entry / download                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *f;
    int64_t total;
    int64_t done;
    int status;
    bool failed;
    const frame_store_ops_t *ops;
} dl_ctx_t;

static void dl_chunk(void *user, const uint8_t *data, size_t len)
{
    dl_ctx_t *d = (dl_ctx_t *)user;
    if (d->f != NULL && fwrite(data, 1, len, d->f) != len) {
        d->failed = true;
    }
    d->done += (int64_t)len;
    if (d->ops != NULL && d->ops->progress != NULL && d->total > 0) {
        int pct = (int)((d->done * 20) / d->total);
        if (pct > 20) {
            pct = 20;
        }
        d->ops->progress(pct, "Downloading", d->ops->user);
    }
}

static void dl_done(void *user, int status, const uint8_t *body, size_t len)
{
    (void)body;
    (void)len;
    ((dl_ctx_t *)user)->status = status;
}

static esp_err_t download_to_staging(const char *url, size_t expected, const frame_store_ops_t *ops)
{
    if (!store_fs_ready()) {
        store_report_error("asset filesystem unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    (void)store_mkdir(STAGING_DIR);
    FILE *f = fopen(STAGING_FPACK, "wb");
    if (f == NULL) {
        store_report_error("cannot open staging file");
        return ESP_FAIL;
    }

    dl_ctx_t d = {
        .f = f, .total = (int64_t)expected, .status = 0, .ops = ops,
    };
    frame_http_req_t req = {
        .url = url,
        .timeout_ms = 30000,
        .on_chunk = dl_chunk,
        .cb = dl_done,
        .user = &d,
    };
    esp_err_t err = frame_http_perform(&req);
    fclose(f);

    if (err != ESP_OK || d.failed || d.status < 200 || d.status >= 300) {
        remove(STAGING_FPACK);
        store_report_error("download failed (HTTP %d)", d.status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t store_install_catalog_entry(const frame_catalog_entry_t *e,
                                      const frame_store_ops_t *ops)
{
    uint8_t pub[32];
    const uint8_t *key = NULL;
    if (!frame_store_unsigned_allowed()) {
        if (e->publisher_key[0] != '\0') {
            esp_err_t err = store_parse_pubkey(e->publisher_key, pub);
            if (err != ESP_OK) {
                store_report_error("bad publisher key for %s", e->manifest.id);
                return ESP_ERR_INVALID_ARG;
            }
            key = pub;
        } else {
            key = frame_store_root_key();
            if (key == NULL) {
                store_report_error("no store root key configured");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    esp_err_t err = download_to_staging(e->download_url, e->package_size, ops);
    if (err != ESP_OK) {
        return err;
    }
    err = store_install_fpack_file(STAGING_FPACK, key, ops);
    remove(STAGING_FPACK);
    return err;
}

/* ------------------------------------------------------------------ */
/* Public install/remove API                                            */
/* ------------------------------------------------------------------ */

esp_err_t frame_store_install(const char *app_id, const frame_store_ops_t *ops)
{
    const frame_catalog_entry_t *e = frame_store_catalog_find(app_id);
    if (e == NULL) {
        store_report_error("%s is not in the catalog", app_id);
        return ESP_ERR_NOT_FOUND;
    }
    return store_install_catalog_entry(e, ops);
}

esp_err_t frame_store_install_file(const char *path, const frame_store_ops_t *ops)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Sideloads are only trusted if the store root signed them. */
    return store_install_fpack_file(path, frame_store_root_key(), ops);
}

esp_err_t frame_store_install_bytes(const uint8_t *data, size_t len, const frame_store_ops_t *ops)
{
    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_fs_ready()) {
        store_report_error("asset filesystem unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    (void)store_mkdir(STAGING_DIR);
    esp_err_t err = store_file_write(STAGING_FPACK, data, len);
    if (err != ESP_OK) {
        return err;
    }
    err = store_install_fpack_file(STAGING_FPACK, frame_store_root_key(), ops);
    remove(STAGING_FPACK);
    return err;
}

esp_err_t frame_store_remove(const char *app_id)
{
    const frame_app_state_t *app = frame_app_manager_find(app_id);
    if (app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (app->builtin) {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (app->kind == FRAME_APP_KIND_NATIVE) {
        const int slot = frame_ota_find_slot(app_id);
        if (slot >= 0) {
            if (frame_ota_erase_slot(slot) != ESP_OK) {
                ESP_LOGW(TAG, "slot %d not erased; will be reclaimed later", slot);
            }
            frame_ota_release_slot(slot);
        }
    } else {
        char path[FRAME_APP_PATH_MAX + 40];
        char dir[FRAME_APP_PATH_MAX + 32];
        snprintf(dir, sizeof(dir), FRAME_STORE_APPS_DIR "/%s", app_id);
        snprintf(path, sizeof(path), "%s/app.wasm", dir);
        remove(path);
        snprintf(path, sizeof(path), "%s/manifest.json", dir);
        remove(path);
        snprintf(path, sizeof(path), "%s/assets", dir);
        rmdir(path);
        rmdir(dir);
    }

    frame_registry_remove(app_id);
    frame_app_manager_remove(app_id);

    frame_event_t e = { .type = FRAME_EVT_APP_REMOVED };
    store_copy(e.text, sizeof(e.text), app_id);
    (void)frame_event_post(&e);
    FRAME_LOGI(TAG, "removed %s", app_id);
    return ESP_OK;
}
