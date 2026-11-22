/*
 * frame_storage/storage.h — NVS settings, the FAT asset area, and the app
 * registry that survives reboots.
 *
 * Ordering matters throughout this component. The registry entry is the single
 * source of truth for what is installed, and it is always written *after* the
 * thing it describes has been committed. A power cut at any point therefore
 * leaves an orphaned slot (reclaimable) rather than a registry entry pointing
 * at erased flash (broken).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "frame_core/app_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Boot                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t frame_storage_init(void);

/* Mounts the FAT partition used for icons, assets and download staging.
 * Non-fatal: the device still works without it, just without app assets. */
esp_err_t frame_storage_mount_fs(void);
bool      frame_storage_fs_ready(void);

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

esp_err_t frame_settings_set_str(const char *key, const char *val);
esp_err_t frame_settings_get_str(const char *key, char *out, size_t cap, const char *dflt);

esp_err_t frame_settings_set_u32(const char *key, uint32_t val);
esp_err_t frame_settings_get_u32(const char *key, uint32_t *out, uint32_t dflt);

esp_err_t frame_settings_set_bool(const char *key, bool val);
esp_err_t frame_settings_get_bool(const char *key, bool *out, bool dflt);

esp_err_t frame_settings_set_blob(const char *key, const void *buf, size_t len);
esp_err_t frame_settings_get_blob(const char *key, void *buf, size_t cap, size_t *len_out);

esp_err_t frame_settings_erase(const char *key);
void      frame_settings_reset_all(void);

/* ------------------------------------------------------------------ */
/* App registry                                                         */
/* ------------------------------------------------------------------ */

esp_err_t frame_registry_open(void);
esp_err_t frame_registry_save(const frame_app_state_t *state);
esp_err_t frame_registry_remove(const char *id);

/* Loads every persisted entry and registers it with the app manager.
 * Returns the number of apps restored. */
size_t frame_registry_load_all(void);

/* Enumerates persisted app ids. Returns the number written. */
size_t frame_registry_list(char ids[][FRAME_APP_ID_MAX], size_t max);

#ifdef __cplusplus
}
#endif
