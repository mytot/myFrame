#include "frame_store/store.h"
#include "store_internal.h"

#include "frame_core/app.h"
#include "frame_core/board_types.h"
#include "frame_core/event.h"
#include "frame_display/display.h"
#include "frame_log/log.h"
#include "frame_ota/ota.h"
#include "frame_pure/base64url.h"
#include "frame_pure/semver.h"
#include "frame_storage/storage.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "frame_store_root_gen.h"

static const char *TAG = "store";

#ifndef CONFIG_FRAME_STORE_ALLOW_UNSIGNED
#define CONFIG_FRAME_STORE_ALLOW_UNSIGNED 0
#endif
#ifndef CONFIG_FRAME_STORE_URL
#define CONFIG_FRAME_STORE_URL "http://192.168.1.10:8080"
#endif

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

static char      s_url[FRAME_STORE_URL_MAX] = CONFIG_FRAME_STORE_URL;
static uint8_t   s_root_key[32];
static bool      s_root_key_present;
static bool      s_settings_loaded;

esp_err_t store_settings_load(void)
{
    if (s_settings_loaded) {
        return ESP_OK;
    }
    s_settings_loaded = true;

    char url[FRAME_STORE_URL_MAX];
    if (frame_settings_get_str("store_url", url, sizeof(url), NULL) == ESP_OK && url[0] != '\0') {
        store_copy(s_url, sizeof(s_url), url);
    }

    uint8_t key[32];
    size_t len = 0;
    if (frame_settings_get_blob("store_root_key", key, sizeof(key), &len) == ESP_OK &&
        len == sizeof(key)) {
        /* A key stored at runtime overrides the compiled-in one. */
        memcpy(s_root_key, key, sizeof(key));
        s_root_key_present = true;
    } else {
#if !FRAME_STORE_ROOT_KEY_NONE
        memcpy(s_root_key, FRAME_STORE_ROOT_KEY, 32);
        s_root_key_present = true;
#else
        s_root_key_present = false;
#endif
    }
    return ESP_OK;
}

esp_err_t store_settings_save_str(const char *key, const char *val)
{
    return frame_settings_set_str(key, val);
}

void store_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1u);
    dst[cap - 1u] = '\0';
}

long store_b64_decode(const char *text, uint8_t *out, size_t cap)
{
    if (text == NULL || out == NULL) {
        return -1;
    }
    return frame_b64url_decode(text, out, cap);
}

esp_err_t store_parse_pubkey(const char *text, uint8_t pub[32])
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const char *b64 = text;
    /* Accept "ed25519:<b64>" and "ed25519 <b64>". */
    const char *colon = strchr(text, ':');
    if (colon != NULL) {
        b64 = colon + 1;
    }
    const long n = store_b64_decode(b64, pub, 32);
    return (n == 32) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------ */
/* FAT helpers                                                          */
/* ------------------------------------------------------------------ */

bool store_fs_ready(void)
{
    return frame_storage_fs_ready();
}

esp_err_t store_mkdir(const char *path)
{
    if (!store_fs_ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return (mkdir(path, 0755) == 0) ? ESP_OK : ESP_FAIL;
}

bool store_file_exists(const char *path)
{
    struct stat st;
    return store_fs_ready() && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t store_file_write(const char *path, const uint8_t *data, size_t len)
{
    if (!store_fs_ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    const size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return (n == len) ? ESP_OK : ESP_FAIL;
}

long store_file_read(const char *path, uint8_t *buf, size_t cap)
{
    if (!store_fs_ready()) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

/* ------------------------------------------------------------------ */
/* Errors                                                               */
/* ------------------------------------------------------------------ */

void store_report_error(const char *fmt, ...)
{
    char msg[FRAME_EVENT_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    FRAME_LOGE(TAG, "%s", msg);
    frame_event_t e = { .type = FRAME_EVT_APP_ERROR };
    store_copy(e.text, sizeof(e.text), msg);
    (void)frame_event_post(&e);
}

/* ------------------------------------------------------------------ */
/* Public settings API                                                  */
/* ------------------------------------------------------------------ */

const char *frame_store_url(void)
{
    store_settings_load();
    return s_url;
}

esp_err_t frame_store_set_url(const char *url)
{
    if (url == NULL || strlen(url) >= FRAME_STORE_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    store_copy(s_url, sizeof(s_url), url);
    return frame_settings_set_str("store_url", url);
}

const uint8_t *frame_store_root_key(void)
{
    store_settings_load();
    return s_root_key_present ? s_root_key : NULL;
}

bool frame_store_root_key_present(void)
{
    store_settings_load();
    return s_root_key_present;
}

esp_err_t frame_store_set_root_key(const uint8_t pub[32])
{
    if (pub == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = frame_settings_set_blob("store_root_key", pub, 32);
    if (err == ESP_OK) {
        memcpy(s_root_key, pub, 32);
        s_root_key_present = true;
    }
    return err;
}

esp_err_t frame_store_root_key_b64(char *out, size_t cap)
{
    store_settings_load();
    if (!s_root_key_present) {
        return ESP_ERR_NOT_FOUND;
    }
    return (frame_b64url_encode(s_root_key, 32, out, cap) > 0) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool frame_store_unsigned_allowed(void)
{
    return CONFIG_FRAME_STORE_ALLOW_UNSIGNED != 0;
}

esp_err_t frame_store_device_caps(frame_device_caps_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const frame_board_t *b = frame_board();
    memset(out, 0, sizeof(*out));
    out->chip = frame_chip_str(b->chip);
    out->psram_mb = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024u * 1024u));
    if (b->display != NULL) {
        out->width = b->display->width;
        out->height = b->display->height;
    }
    out->touch = (b->touch != NULL && b->touch->controller != FRAME_TOUCH_NONE);
    out->audio = (b->audio != NULL && b->audio->codec != FRAME_AUDIO_NONE);
    out->sdk_version = FRAME_SDK_VERSION;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Catalog glue (implemented in catalog.c)                              */
/* ------------------------------------------------------------------ */

esp_err_t store_catalog_load_cached(void);
esp_err_t store_catalog_fetch(frame_store_progress_cb_t cb, void *user);
esp_err_t store_catalog_clear(void);
const char *store_catalog_last_error(void);

/* ------------------------------------------------------------------ */
/* Install glue (implemented in install.c)                              */
/* ------------------------------------------------------------------ */

esp_err_t store_install_fpack_file(const char *path, const uint8_t pub[32],
                                   const frame_store_ops_t *ops);
esp_err_t store_install_catalog_entry(const frame_catalog_entry_t *e,
                                      const frame_store_ops_t *ops);
esp_err_t store_erase_stale_slots(unsigned max_slots);

/* ------------------------------------------------------------------ */
/* Reconcile                                                            */
/* ------------------------------------------------------------------ */

static bool slot_referenced(const char *app_id, void *ctx)
{
    (void)ctx;
    return frame_app_manager_find(app_id) != NULL;
}

esp_err_t frame_store_reconcile(void)
{
    size_t dropped = 0;
    for (size_t i = 0; i < FRAME_APP_MAX; i++) {
        /* Iterate by index and re-find by id each round: removal shifts the array. */
        if (i >= frame_app_manager_count()) {
            break;
        }
        const frame_app_state_t *app = frame_app_manager_at(i);
        if (app == NULL || app->builtin) {
            continue;
        }
        bool ok = true;
        if (app->kind == FRAME_APP_KIND_NATIVE) {
            ok = (frame_ota_find_slot(app->id) >= 0);
        } else if (app->kind == FRAME_APP_KIND_MANAGED) {
            char path[FRAME_APP_PATH_MAX + 32];
            ok = (frame_store_managed_path(app->id, path, sizeof(path)) == ESP_OK) &&
                 store_file_exists(path);
        }
        if (!ok) {
            ESP_LOGW(TAG, "registry entry %s has no backing — removing", app->id);
            frame_registry_remove(app->id);
            frame_app_manager_remove(app->id);
            dropped++;
            /* Re-examine this index: the next entry moved into it. */
            i--;
        }
    }

    /* Reclaim reservations abandoned by a power cut and demote VALID slots
     * nobody references any more. Erasing them is deferred to the store task. */
    frame_ota_sweep(RESERVATION_EXPIRY_MS, slot_referenced, NULL);

    if (dropped > 0u) {
        ESP_LOGI(TAG, "reconcile: dropped %u orphaned registry entr%s",
                 (unsigned)dropped, dropped == 1u ? "y" : "ies");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Task                                                                 */
/* ------------------------------------------------------------------ */

#define STORE_QUEUE_LEN 4u
#define STORE_TASK_STACK 12288u

static QueueHandle_t      s_queue;
static TaskHandle_t       s_task;
static frame_store_state_t s_state;

frame_store_state_t frame_store_state(void)
{
    return s_state;
}

const char *frame_store_state_str(frame_store_state_t s)
{
    switch (s) {
    case FRAME_STORE_IDLE:        return "idle";
    case FRAME_STORE_REFRESHING:  return "refreshing";
    case FRAME_STORE_DOWNLOADING: return "downloading";
    case FRAME_STORE_INSTALLING:  return "installing";
    case FRAME_STORE_REMOVING:    return "removing";
    case FRAME_STORE_FAILED:      return "failed";
    default:                      return "?";
    }
}

static void set_state(frame_store_state_t st)
{
    s_state = st;
    FRAME_LOGD(TAG, "state -> %s", frame_store_state_str(st));
}

static void handle_request(const frame_store_request_t *r)
{
    frame_store_ops_t ops = { .progress = r->progress, .user = r->user };

    switch (r->cmd) {
    case FRAME_STORE_CMD_REFRESH:
        set_state(FRAME_STORE_REFRESHING);
        if (frame_store_refresh(r->progress, r->user) != ESP_OK) {
            set_state(FRAME_STORE_FAILED);
        }
        break;
    case FRAME_STORE_CMD_INSTALL:
        set_state(FRAME_STORE_DOWNLOADING);
        if (frame_store_install(r->id, &ops) != ESP_OK) {
            set_state(FRAME_STORE_FAILED);
        }
        break;
    case FRAME_STORE_CMD_INSTALL_FILE:
        set_state(FRAME_STORE_INSTALLING);
        if (frame_store_install_file(r->id, &ops) != ESP_OK) {
            set_state(FRAME_STORE_FAILED);
        }
        break;
    case FRAME_STORE_CMD_REMOVE:
        set_state(FRAME_STORE_REMOVING);
        if (frame_store_remove(r->id) != ESP_OK) {
            set_state(FRAME_STORE_FAILED);
        }
        break;
    default:
        break;
    }
    if (s_state != FRAME_STORE_FAILED) {
        set_state(FRAME_STORE_IDLE);
    }
}

static void store_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "store task on core %d", (int)xPortGetCoreID());

    /* Boot-time garbage collection: erase at most one stale slot so a slow
     * flash erase never delays the launcher. The rest go on later boots or
     * after the next install. */
    (void)store_erase_stale_slots(1u);
    set_state(FRAME_STORE_IDLE);

    frame_store_request_t r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        handle_request(&r);
    }
}

esp_err_t frame_store_request(const frame_store_request_t *req)
{
    if (req == NULL || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != FRAME_STORE_IDLE && s_state != FRAME_STORE_FAILED) {
        return ESP_ERR_INVALID_STATE;
    }
    frame_store_request_t copy = *req;
    if (xQueueSend(s_queue, &copy, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t frame_store_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    store_settings_load();

    s_queue = xQueueCreate(STORE_QUEUE_LEN, sizeof(frame_store_request_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        store_task, "frame_store", STORE_TASK_STACK, NULL, 3, &s_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Make the offline catalog usable immediately, before any network job. */
    (void)store_catalog_load_cached();

    ESP_LOGI(TAG, "store ready (root key %s, unsigned %s)",
             s_root_key_present ? "present" : "absent",
             frame_store_unsigned_allowed() ? "allowed" : "refused");
    return ESP_OK;
}

esp_err_t frame_store_managed_path(const char *app_id, char *out, size_t cap)
{
    if (app_id == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, cap, FRAME_STORE_APPS_DIR "/%s/app.wasm", app_id);
    if (n < 0 || (size_t)n >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* App ids are reverse-DNS; refuse path separators so a hostile manifest
     * cannot write outside the apps directory. */
    if (strchr(app_id, '/') != NULL || strstr(app_id, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
