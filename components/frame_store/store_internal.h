/*
 * frame_store/store_internal.h — shared between catalog.c, install.c and
 * store.c. Not installed: nothing outside this component needs it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "frame_store/store.h"

#define STORE_TAG                "store"
#define STORE_NVS_NS             "framestore"
#define STORE_KEY_URL            "url"
#define STORE_KEY_ROOT           "rootkey"

#define STAGING_DIR              FRAME_STORE_APPS_DIR "/.staging"
#define STAGING_FPACK            STAGING_DIR "/pkg.fpack"
#define CATALOG_CACHE_JSON       FRAME_STORE_APPS_DIR "/.catalog.json"
#define CATALOG_CACHE_SIG        FRAME_STORE_APPS_DIR "/.catalog.sig"

#define MANIFEST_MAX_BYTES       8192u
#define RESERVATION_EXPIRY_MS    (10u * 60u * 1000u)

/* Progress helpers: the callback is optional everywhere. */
static inline void store_progress(const frame_store_ops_t *ops, int pct, const char *stage)
{
    if (ops != NULL && ops->progress != NULL) {
        ops->progress(pct, stage, ops->user);
    }
}

/* Base64/base64url text -> raw bytes. Returns length or a negative value. */
long store_b64_decode(const char *text, uint8_t *out, size_t cap);

/* Decodes "ed25519:<b64>" or a bare b64 string into a 32-byte key. */
esp_err_t store_parse_pubkey(const char *text, uint8_t pub[32]);

/* NVS-backed settings (namespace "framestore"). */
esp_err_t store_settings_load(void);
esp_err_t store_settings_save_str(const char *key, const char *val);

/* FAT helpers; all no-ops (ESP_ERR_NOT_SUPPORTED) when the FS is not mounted. */
bool     store_fs_ready(void);
esp_err_t store_mkdir(const char *path);
bool     store_file_exists(const char *path);
esp_err_t store_file_write(const char *path, const uint8_t *data, size_t len);
long     store_file_read(const char *path, uint8_t *buf, size_t cap);

/* Announces a failure to the UI without blocking the store task. */
void store_report_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies with truncation, always NUL-terminates. */
void store_copy(char *dst, size_t cap, const char *src);
