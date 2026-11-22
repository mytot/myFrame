/*
 * catalog.c — fetch, verify, parse, cache and diff the app catalog.
 *
 * The catalog is only ever trusted after its detached Ed25519 signature
 * verifies against the pinned store root key. The raw bytes are cached on
 * FAT so the store still opens offline; the cache is re-verified on load, so
 * tampering with the file is no more useful than tampering with the network.
 */
#include "frame_store/store.h"
#include "store_internal.h"

#include "frame_core/app.h"
#include "frame_core/event.h"
#include "frame_log/log.h"
#include "frame_net/net.h"
#include "frame_pure/canonical.h"
#include "frame_pure/ed25519.h"
#include "frame_pure/json.h"
#include "frame_pure/semver.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "catalog";

typedef struct {
    frame_catalog_entry_t *entries;
    size_t                 count;
    bool                   valid;
    int64_t                fetched_at_us;
    char                   last_error[FRAME_EVENT_TEXT_MAX];
} catalog_state_t;

static catalog_state_t s_cat;

/* ------------------------------------------------------------------ */
/* HTTP body capture                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      status;
    bool     overflow;
} body_t;

static void body_chunk(void *user, const uint8_t *data, size_t len)
{
    body_t *b = (body_t *)user;
    if (b->len + len > b->cap) {
        b->overflow = true;
        return;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

static void body_done(void *user, int status, const uint8_t *body, size_t len)
{
    (void)body;
    (void)len;
    body_t *b = (body_t *)user;
    b->status = status;
}

/* GET url into a PSRAM buffer. Returns the body or NULL. Caller frees. */
static uint8_t *http_get(const char *url, size_t cap, size_t *len_out, int *status_out)
{
    body_t b = { 0 };
    b.buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b.buf == NULL) {
        b.buf = malloc(cap);
    }
    if (b.buf == NULL) {
        return NULL;
    }
    b.cap = cap;

    frame_http_req_t req = {
        .url = url,
        .timeout_ms = 15000,
        .on_chunk = body_chunk,
        .cb = body_done,
        .user = &b,
    };
    esp_err_t err = frame_http_perform(&req);
    if (status_out != NULL) {
        *status_out = (err == ESP_OK) ? b.status : -1;
    }
    if (err != ESP_OK || b.status < 200 || b.status >= 300 || b.overflow) {
        free(b.buf);
        return NULL;
    }
    *len_out = b.len;
    return b.buf;
}

/* ------------------------------------------------------------------ */
/* Signature                                                            */
/* ------------------------------------------------------------------ */

static bool verify_catalog_sig(const uint8_t *json, size_t json_len, const char *sig_text)
{
    if (frame_store_unsigned_allowed()) {
        FRAME_LOGW(TAG, "dev mode: catalog signature not enforced");
        return true;
    }
    const uint8_t *root = frame_store_root_key();
    if (root == NULL) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "no store root key configured");
        return false;
    }
    uint8_t sig[64];
    if (store_b64_decode(sig_text, sig, sizeof(sig)) != 64) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "catalog signature malformed");
        return false;
    }
    if (frame_ed25519_verify(sig, root, json, json_len) != 0) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "catalog signature invalid");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Parse                                                                */
/* ------------------------------------------------------------------ */

static bool parse_catalog(const char *json, size_t len)
{
    /* The arena is sized generously: frame_pure/json allocates nodes and
     * string copies from it, and a full catalog is < 256 KB of text. */
    const size_t arena_cap = len * 6u + 4096u;
    void *arena = heap_caps_malloc(arena_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (arena == NULL) {
        arena = malloc(arena_cap);
    }
    frame_catalog_entry_t *entries = heap_caps_malloc(
        sizeof(frame_catalog_entry_t) * FRAME_STORE_MAX_APPS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (arena == NULL || entries == NULL) {
        free(arena);
        free(entries);
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "out of memory parsing catalog");
        return false;
    }

    fj_arena_t a;
    fj_arena_init(&a, arena, arena_cap);
    fj_val_t *root = fj_parse(json, len, &a);
    size_t count = 0;

    if (root == NULL) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "catalog is not valid JSON");
        goto fail;
    }
    if (fj_as_int(fj_obj_get(root, "schema"), 0) != 1) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "unsupported catalog schema");
        goto fail;
    }
    fj_val_t *apps = fj_obj_get(root, "apps");
    if (apps == NULL) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "catalog has no apps array");
        goto fail;
    }

    const size_t n = fj_arr_count(apps);
    for (size_t i = 0; i < n && count < FRAME_STORE_MAX_APPS; i++) {
        fj_val_t *item = fj_arr_at(apps, i);
        fj_val_t *m = fj_obj_get(item, "manifest");
        if (item == NULL || m == NULL) {
            continue;
        }

        /* Re-serialise the embedded manifest and parse it with the same
         * parser the device uses for a package, so both paths agree on what
         * a valid manifest is. */
        char mtext[MANIFEST_MAX_BYTES];
        frame_canon_t c;
        frame_canon_init(&c, mtext, sizeof(mtext));
        if (!frame_canon_write(&c, m) || c.overflow) {
            continue;
        }

        frame_catalog_entry_t *e = &entries[count];
        memset(e, 0, sizeof(*e));
        if (frame_manifest_parse(mtext, strlen(mtext), &e->manifest) != FRAME_MANIFEST_OK) {
            continue;
        }
        if (!fj_str_copy(fj_obj_get(item, "download_url"), e->download_url, sizeof(e->download_url))) {
            continue;
        }
        fj_str_copy(fj_obj_get(item, "publisher_key"), e->publisher_key, sizeof(e->publisher_key));
        fj_str_copy(fj_obj_get(item, "publisher_alg"), e->publisher_alg, sizeof(e->publisher_alg));
        fj_str_copy(fj_obj_get(item, "sha256"), e->package_sha256, sizeof(e->package_sha256));
        e->package_size = (uint32_t)fj_as_int(fj_obj_get(item, "size"), 0);
        count++;
    }

    if (count == 0u) {
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "catalog contained no usable apps");
        goto fail;
    }

    free(s_cat.entries);
    s_cat.entries = entries;
    s_cat.count   = count;
    s_cat.valid   = true;
    s_cat.fetched_at_us = esp_timer_get_time();
    free(arena);
    ESP_LOGI(TAG, "catalog: %u apps", (unsigned)count);
    return true;

fail:
    free(arena);
    free(entries);
    return false;
}

/* ------------------------------------------------------------------ */
/* Load / cache                                                         */
/* ------------------------------------------------------------------ */

static bool load_from_buffers(const uint8_t *json, size_t json_len, const char *sig)
{
    if (json_len == 0u || json_len > FRAME_STORE_CATALOG_MAX) {
        return false;
    }
    /* NUL-terminate for the parser (it takes a length, but fj_parse may
     * need a terminator for some error paths). */
    char *copy = malloc(json_len + 1u);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, json, json_len);
    copy[json_len] = '\0';

    bool ok = verify_catalog_sig(json, json_len, sig) && parse_catalog(copy, json_len);
    free(copy);
    return ok;
}

esp_err_t store_catalog_load_cached(void)
{
    if (!store_fs_ready() || !store_file_exists(CATALOG_CACHE_JSON)) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *json = malloc(FRAME_STORE_CATALOG_MAX);
    long json_len = store_file_read(CATALOG_CACHE_JSON, json, FRAME_STORE_CATALOG_MAX);
    char sig[128] = { 0 };
    long sig_len = store_file_read(CATALOG_CACHE_SIG, (uint8_t *)sig, sizeof(sig) - 1u);

    esp_err_t rc = ESP_ERR_NOT_FOUND;
    if (json_len > 0 && sig_len > 0) {
        sig[sig_len] = '\0';
        /* Trim whitespace/newlines. */
        for (char *p = sig; *p != '\0'; p++) {
            if (*p == '\n' || *p == '\r' || *p == ' ') {
                *p = '\0';
                break;
            }
        }
        if (load_from_buffers(json, (size_t)json_len, sig)) {
            ESP_LOGI(TAG, "cached catalog loaded (%ld bytes)", json_len);
            rc = ESP_OK;
        } else {
            ESP_LOGW(TAG, "cached catalog rejected: %s", s_cat.last_error);
            rc = ESP_ERR_INVALID_CRC;
        }
    }
    free(json);
    return rc;
}

static void cache_catalog(const uint8_t *json, size_t len, const char *sig)
{
    if (!store_fs_ready()) {
        return;
    }
    if (store_file_write(CATALOG_CACHE_JSON, json, len) == ESP_OK &&
        store_file_write(CATALOG_CACHE_SIG, (const uint8_t *)sig, strlen(sig)) == ESP_OK) {
        ESP_LOGI(TAG, "catalog cached for offline use");
    } else {
        ESP_LOGW(TAG, "could not cache catalog (FS write failed)");
    }
}

esp_err_t store_catalog_fetch(frame_store_progress_cb_t cb, void *user)
{
    char url[FRAME_STORE_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/catalog.json", frame_store_url());

    if (cb != NULL) {
        cb(5, "Fetching catalog", user);
    }
    size_t json_len = 0;
    int status = 0;
    uint8_t *json = http_get(url, FRAME_STORE_CATALOG_MAX, &json_len, &status);
    if (json == NULL) {
        snprintf(s_cat.last_error, sizeof(s_cat.last_error), "catalog fetch failed (HTTP %d)", status);
        ESP_LOGW(TAG, "%s", s_cat.last_error);
        store_report_error("%s", s_cat.last_error);
        return ESP_FAIL;
    }

    snprintf(url, sizeof(url), "%s/catalog.sig", frame_store_url());
    size_t sig_len = 0;
    uint8_t *sig = http_get(url, 512, &sig_len, &status);
    if (cb != NULL) {
        cb(30, "Verifying catalog", user);
    }
    if (sig == NULL) {
        free(json);
        store_copy(s_cat.last_error, sizeof(s_cat.last_error), "catalog signature missing");
        store_report_error("%s", s_cat.last_error);
        return ESP_FAIL;
    }

    /* Trim trailing whitespace from the signature file. */
    while (sig_len > 0u && (sig[sig_len - 1u] == '\n' || sig[sig_len - 1u] == '\r' ||
                            sig[sig_len - 1u] == ' ')) {
        sig_len--;
    }
    uint8_t *sig_z = realloc(sig, sig_len + 1u);
    if (sig_z == NULL) {
        free(json);
        free(sig);
        return ESP_ERR_NO_MEM;
    }
    sig_z[sig_len] = '\0';

    if (!verify_catalog_sig(json, json_len, (const char *)sig_z)) {
        ESP_LOGE(TAG, "%s", s_cat.last_error);
        store_report_error("%s", s_cat.last_error);
        free(json);
        free(sig_z);
        return ESP_ERR_INVALID_CRC;
    }

    char *copy = malloc(json_len + 1u);
    if (copy == NULL) {
        free(json);
        free(sig_z);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, json, json_len);
    copy[json_len] = '\0';

    if (cb != NULL) {
        cb(60, "Reading catalog", user);
    }
    const bool ok = parse_catalog(copy, json_len);
    free(copy);

    if (!ok) {
        ESP_LOGE(TAG, "%s", s_cat.last_error);
        store_report_error("%s", s_cat.last_error);
        free(json);
        free(sig_z);
        return ESP_FAIL;
    }
    /* Cache the exact bytes that were verified, not a re-serialisation. */
    cache_catalog(json, json_len, (const char *)sig_z);
    free(json);
    free(sig_z);
    return ESP_OK;
}

esp_err_t store_catalog_clear(void)
{
    s_cat.valid = false;
    return ESP_OK;
}

const char *store_catalog_last_error(void)
{
    return s_cat.last_error;
}

/* ------------------------------------------------------------------ */
/* Public catalog API                                                   */
/* ------------------------------------------------------------------ */

bool frame_store_catalog_valid(void)
{
    return s_cat.valid;
}

size_t frame_store_catalog_count(void)
{
    return s_cat.count;
}

const frame_catalog_entry_t *frame_store_catalog_at(size_t index)
{
    return (index < s_cat.count) ? &s_cat.entries[index] : NULL;
}

const frame_catalog_entry_t *frame_store_catalog_find(const char *app_id)
{
    if (app_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s_cat.count; i++) {
        if (strncmp(s_cat.entries[i].manifest.id, app_id, sizeof(s_cat.entries[i].manifest.id)) == 0) {
            return &s_cat.entries[i];
        }
    }
    return NULL;
}

esp_err_t frame_store_refresh(frame_store_progress_cb_t cb, void *user)
{
    s_cat.last_error[0] = '\0';
    esp_err_t err = store_catalog_fetch(cb, user);
    if (cb != NULL) {
        cb(100, err == ESP_OK ? "Catalog updated" : "Catalog unavailable", user);
    }
    if (err == ESP_OK) {
        (void)frame_store_check_updates();
    }
    return err;
}

uint32_t frame_store_check_updates(void)
{
    uint32_t updates = 0;
    if (!s_cat.valid) {
        return 0;
    }
    for (size_t i = 0; i < s_cat.count; i++) {
        const frame_catalog_entry_t *e = &s_cat.entries[i];
        const frame_app_state_t *installed = frame_app_manager_find(e->manifest.id);
        if (installed == NULL) {
            continue;
        }
        (void)installed;
        if (frame_app_manager_set_available(e->manifest.id, e->manifest.version) == ESP_OK) {
            const frame_app_state_t *after = frame_app_manager_find(e->manifest.id);
            if (after != NULL && after->update_available) {
                updates++;
            }
        }
    }
    updates = frame_app_manager_update_count();
    if (updates > 0u) {
        ESP_LOGI(TAG, "%u update(s) available", (unsigned)updates);
    }
    (void)frame_event_post_simple(FRAME_EVT_STORE_UPDATED, (int32_t)updates, 0);
    return updates;
}
