#include "frame_storage/storage.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "registry";

#define NVS_NS       "frameapps"
#define KEY_PREFIX   "a:"

static nvs_handle_t s_handle;
static bool         s_ready;

esp_err_t frame_registry_open(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

static void make_key(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, "%s%s", KEY_PREFIX, id);
}

esp_err_t frame_registry_save(const frame_app_state_t *state)
{
    if (state == NULL || state->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = frame_registry_open();
    if (err != ESP_OK) {
        return err;
    }

    char key[FRAME_APP_ID_MAX + 8];
    make_key(key, sizeof(key), state->id);

    err = nvs_set_blob(s_handle, key, state, sizeof(*state));
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save %s: %s", state->id, esp_err_to_name(err));
    }
    return err;
}

esp_err_t frame_registry_remove(const char *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = frame_registry_open();
    if (err != ESP_OK) {
        return err;
    }

    char key[FRAME_APP_ID_MAX + 8];
    make_key(key, sizeof(key), id);

    err = nvs_erase_key(s_handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(s_handle);
    }
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

size_t frame_registry_list(char ids[][FRAME_APP_ID_MAX], size_t max)
{
    if (ids == NULL || max == 0u) {
        return 0u;
    }
    if (frame_registry_open() != ESP_OK) {
        return 0u;
    }

    size_t written = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_NS, NULL, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it != NULL && written < max) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, KEY_PREFIX, strlen(KEY_PREFIX)) == 0) {
            const char *id = info.key + strlen(KEY_PREFIX);
            if (strlen(id) < FRAME_APP_ID_MAX) {
                memcpy(ids[written], id, strlen(id) + 1u);
                written++;
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it != NULL) {
        nvs_release_iterator(it);
    }
    return written;
}

size_t frame_registry_load_all(void)
{
    char ids[FRAME_APP_MAX][FRAME_APP_ID_MAX];
    size_t n = frame_registry_list(ids, FRAME_APP_MAX);
    size_t loaded = 0;

    for (size_t i = 0; i < n; i++) {
        char key[FRAME_APP_ID_MAX + 8];
        make_key(key, sizeof(key), ids[i]);

        frame_app_state_t st;
        size_t len = sizeof(st);
        if (nvs_get_blob(s_handle, key, &st, &len) != ESP_OK || len != sizeof(st)) {
            /* A blob of the wrong size is from an incompatible build. Drop it
             * rather than half-loading it. */
            ESP_LOGW(TAG, "registry entry %s has unexpected size — dropping", ids[i]);
            nvs_erase_key(s_handle, key);
            continue;
        }
        if (frame_app_manager_upsert(&st) == ESP_OK) {
            loaded++;
        }
    }

    if (loaded > 0u) {
        ESP_LOGI(TAG, "restored %u apps from the registry", (unsigned)loaded);
    }
    return loaded;
}
