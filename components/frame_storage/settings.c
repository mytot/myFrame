#include "frame_storage/storage.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "settings";

#define NVS_NS "frame"

static nvs_handle_t s_handle;
static SemaphoreHandle_t s_lock;
static bool s_ready;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t frame_storage_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The namespace is reformatted rather than the whole chip: a corrupt
         * partition must not cost the user their installed apps. */
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    err = nvs_open(NVS_NS, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t frame_settings_set_str(const char *key, const char *val)
{
    if (!s_ready || key == NULL || val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_set_str(s_handle, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    unlock();
    return err;
}

esp_err_t frame_settings_get_str(const char *key, char *out, size_t cap, const char *dflt)
{
    if (!s_ready || key == NULL || out == NULL || cap == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    size_t len = cap;
    esp_err_t err = nvs_get_str(s_handle, key, out, &len);
    unlock();
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (dflt != NULL) {
        size_t n = strlen(dflt);
        if (n >= cap) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, dflt, n + 1u);
        return ESP_OK;
    }
    return err;
}

esp_err_t frame_settings_set_u32(const char *key, uint32_t val)
{
    if (!s_ready || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_set_u32(s_handle, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    unlock();
    return err;
}

esp_err_t frame_settings_get_u32(const char *key, uint32_t *out, uint32_t dflt)
{
    if (!s_ready || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_get_u32(s_handle, key, out);
    unlock();
    if (err != ESP_OK) {
        *out = dflt;
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    return ESP_OK;
}

esp_err_t frame_settings_set_bool(const char *key, bool val)
{
    if (!s_ready || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_set_u8(s_handle, key, val ? 1u : 0u);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    unlock();
    return err;
}

esp_err_t frame_settings_get_bool(const char *key, bool *out, bool dflt)
{
    uint8_t v = dflt ? 1u : 0u;
    if (!s_ready || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_get_u8(s_handle, key, &v);
    unlock();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        *out = dflt;
        return err;
    }
    *out = (v != 0u);
    return ESP_OK;
}

esp_err_t frame_settings_set_blob(const char *key, const void *buf, size_t len)
{
    if (!s_ready || key == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_set_blob(s_handle, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    unlock();
    return err;
}

esp_err_t frame_settings_get_blob(const char *key, void *buf, size_t cap, size_t *len_out)
{
    if (!s_ready || key == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = cap;
    lock();
    esp_err_t err = nvs_get_blob(s_handle, key, buf, &len);
    unlock();
    if (err == ESP_OK && len_out != NULL) {
        *len_out = len;
    }
    return err;
}

esp_err_t frame_settings_erase(const char *key)
{
    if (!s_ready || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = nvs_erase_key(s_handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(s_handle);
    }
    unlock();
    return err;
}

void frame_settings_reset_all(void)
{
    if (!s_ready) {
        return;
    }
    lock();
    nvs_erase_all(s_handle);
    nvs_commit(s_handle);
    unlock();
    ESP_LOGW(TAG, "all settings erased");
}
