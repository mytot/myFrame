/*
 * frame_store/store.h — catalog, install/update/remove, and the device-side
 * HTTP API the dev store talks to.
 *
 * Trust model (docs/APP_STORE.md §3):
 *
 *   store root key (pinned in firmware, or set in Settings → Developer)
 *     └── signs catalog.sig over the raw catalog.json bytes
 *           └── each entry carries the app's publisher public key
 *                 └── publisher signs canonical(manifest) || payload
 *
 * A sideloaded .fpack (no catalog) is accepted only if it verifies against
 * the store root key itself. CONFIG_FRAME_STORE_ALLOW_UNSIGNED turns all of
 * this off for development and is refused by `tools/build.py --release`.
 *
 * Everything here blocks. Call it from the store task (frame_store_request*)
 * never from the LVGL task; progress is reported through callbacks that the
 * UI marshals with frame_ui_dispatch().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "frame_core/app_manager.h"
#include "frame_pure/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_STORE_MAX_APPS        32u
#define FRAME_STORE_URL_MAX         160u
#define FRAME_STORE_CATALOG_MAX     (256u * 1024u)
#define FRAME_STORE_APPS_DIR        "/apps"

typedef struct {
    frame_manifest_t manifest;
    char             download_url[192];
    char             publisher_key[64];   /* base64url(32 bytes), "" = root */
    char             publisher_alg[12];   /* "ed25519" */
    uint32_t         package_size;
    char             package_sha256[65];
} frame_catalog_entry_t;

typedef enum {
    FRAME_STORE_IDLE = 0,
    FRAME_STORE_REFRESHING,
    FRAME_STORE_DOWNLOADING,
    FRAME_STORE_INSTALLING,
    FRAME_STORE_REMOVING,
    FRAME_STORE_FAILED,
} frame_store_state_t;

typedef void (*frame_store_progress_cb_t)(int percent, const char *stage, void *user);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

esp_err_t frame_store_init(void);

/* Reconciles the registry with what is actually on flash/FAT and reclaims
 * slots left behind by a power cut. Call once at boot, after the registry
 * has been loaded and before the launcher is shown. */
esp_err_t frame_store_reconcile(void);

frame_store_state_t frame_store_state(void);
const char         *frame_store_state_str(frame_store_state_t s);

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

const char *frame_store_url(void);
esp_err_t   frame_store_set_url(const char *url);

const uint8_t *frame_store_root_key(void);
bool           frame_store_root_key_present(void);
esp_err_t      frame_store_set_root_key(const uint8_t pub[32]);
esp_err_t      frame_store_root_key_b64(char *out, size_t cap);

bool frame_store_unsigned_allowed(void);

/* ------------------------------------------------------------------ */
/* Catalog (in-memory copy of the last verified catalog)                */
/* ------------------------------------------------------------------ */

bool   frame_store_catalog_valid(void);
size_t frame_store_catalog_count(void);
const frame_catalog_entry_t *frame_store_catalog_at(size_t index);
const frame_catalog_entry_t *frame_store_catalog_find(const char *app_id);

/*
 * Blocking. Fetches catalog.json + catalog.sig, verifies the signature with
 * the root key, parses, caches to FAT for offline use, and re-evaluates the
 * update flags. Posts FRAME_EVT_STORE_UPDATED with the update count.
 */
esp_err_t frame_store_refresh(frame_store_progress_cb_t cb, void *user);

/* Compares catalog versions with installed apps and refreshes the badges. */
uint32_t frame_store_check_updates(void);

/* ------------------------------------------------------------------ */
/* Install / remove (blocking)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    frame_store_progress_cb_t progress;
    void                     *user;
} frame_store_ops_t;

/* Downloads the newest catalog version of app_id and installs it. */
esp_err_t frame_store_install(const char *app_id, const frame_store_ops_t *ops);

/* Sideloads a .fpack already on the FAT filesystem. */
esp_err_t frame_store_install_file(const char *path, const frame_store_ops_t *ops);

esp_err_t frame_store_remove(const char *app_id);

/* Installs an in-memory .fpack (device HTTP API upload). Writes it to the
 * staging area first so the same code path as a download is used. */
esp_err_t frame_store_install_bytes(const uint8_t *data, size_t len, const frame_store_ops_t *ops);

/* Filesystem layout for managed apps: FRAME_STORE_APPS_DIR/<id>/app.wasm */
esp_err_t frame_store_managed_path(const char *app_id, char *out, size_t cap);

/* True when the device itself satisfies a catalog manifest's requirements. */
esp_err_t frame_store_device_caps(frame_device_caps_t *out);

/* ------------------------------------------------------------------ */
/* Background task                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    FRAME_STORE_CMD_REFRESH = 0,
    FRAME_STORE_CMD_INSTALL,
    FRAME_STORE_CMD_INSTALL_FILE,
    FRAME_STORE_CMD_REMOVE,
} frame_store_cmd_t;

typedef struct {
    frame_store_cmd_t       cmd;
    char                    id[FRAME_APP_ID_MAX];
    frame_store_progress_cb_t progress;
    void                   *user;
} frame_store_request_t;

/*
 * Queues work for the store task (core 0, priority 3). Returns immediately;
 * ESP_ERR_INVALID_STATE if the queue is full or a job is already running.
 */
esp_err_t frame_store_request(const frame_store_request_t *req);

/* Runs the HTTP API on port 80 (dev store). */
esp_err_t frame_store_api_start(void);

#ifdef __cplusplus
}
#endif
