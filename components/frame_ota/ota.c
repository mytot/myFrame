#include "frame_ota/ota.h"

#include "frame_core/app.h"
#include "frame_core/event.h"
#include "frame_log/log.h"

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota";

/* App slots are the OTA partitions after the shell's own A/B pair (ota_0,
 * ota_1). The real count comes from the partition table at init so the table
 * in NVS never claims a slot that has no partition behind it. */
#define SLOT_FIRST_SUBTYPE ESP_PARTITION_SUBTYPE_APP_OTA_2
#define SLOT_MAX FRAME_OTA_MAX_SLOTS
static uint8_t s_slot_count;

uint32_t frame_ota_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* NVS keys: "st" is the slot table, "boot" is the shell<->app handoff. */
#define NVS_NAMESPACE "frameota"
#define KEY_TABLE     "st"
#define KEY_HANDOFF   "boot"
#define KEY_SHELL     "shell"   /* label of the shell partition to return to */

static frame_slot_table_t s_table;
static frame_ota_stats_t  s_stats;
static bool                s_ready;
static char                s_launched_app[FRAME_APP_ID_MAX];
static char                s_shell_label[17];

const esp_partition_t *frame_ota_slot_partition(int slot)
{
    if (slot < 0 || slot >= SLOT_MAX) {
        return NULL;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(SLOT_FIRST_SUBTYPE + slot), NULL);
}

static uint8_t detect_slot_count(void)
{
    uint8_t n = 0;
    while (n < SLOT_MAX && frame_ota_slot_partition(n) != NULL) {
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Boot confirmation (rollback)                                         */
/* ------------------------------------------------------------------ */

bool frame_ota_boot_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return false;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return (state == ESP_OTA_IMG_PENDING_VERIFY);
}

esp_err_t frame_ota_confirm_boot(void)
{
    if (!frame_ota_boot_pending()) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "boot confirmed; rollback cancelled");
    } else {
        ESP_LOGW(TAG, "could not confirm boot: %s", esp_err_to_name(err));
    }
    return err;
}

/* ------------------------------------------------------------------ */

esp_err_t frame_ota_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_slot_count = detect_slot_count();
    if (s_slot_count == 0u) {
        ESP_LOGW(TAG, "no app partitions in this partition table");
    }
    ESP_LOGI(TAG, "%u app slot(s) available", (unsigned)s_slot_count);
    frame_slot_table_init(&s_table, s_slot_count);
    s_ready = true;

    esp_err_t err = frame_ota_load();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* A fresh NVS is normal on first boot. */
        ESP_LOGW(TAG, "slot table not loaded: %s", esp_err_to_name(err));
    }
    load_handoff();
    return ESP_OK;
}

esp_err_t frame_ota_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    frame_slot_table_t tmp;
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, KEY_TABLE, &tmp, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(tmp)) {
        s_table = tmp;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t frame_ota_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_TABLE, &s_table, sizeof(s_table));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int frame_ota_reserve_slot(void)
{
    int slot = frame_slot_table_reserve(&s_table, frame_ota_now_ms());
    if (slot >= 0) {
        frame_ota_save();
    }
    return slot;
}

esp_err_t frame_ota_commit_slot(int slot, const char *app_id, const char *version)
{
    return (frame_slot_table_commit(&s_table, (uint8_t)slot, app_id, version) >= 0)
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void frame_ota_release_slot(int slot)
{
    if (slot < 0 || slot >= s_slot_count) {
        return;
    }
    frame_slot_table_release(&s_table, (uint8_t)slot);
    frame_ota_save();
}

int frame_ota_find_slot(const char *app_id)
{
    return frame_slot_table_find(&s_table, app_id);
}

void frame_ota_touch_slot(int slot)
{
    if (slot >= 0 && slot < s_slot_count) {
        frame_slot_table_touch(&s_table, (uint8_t)slot, frame_ota_now_ms());
    }
}

void frame_ota_sweep(uint32_t expiry_ms, bool (*is_referenced)(const char *, void *), void *ctx)
{
    frame_slot_table_sweep(&s_table, frame_ota_now_ms(), expiry_ms, is_referenced, ctx);
    frame_ota_save();
}

int frame_ota_evict_lru(const char *protect_id)
{
    return frame_slot_table_evict_lru(&s_table, protect_id);
}

esp_err_t frame_ota_erase_slot(int slot)
{
    const esp_partition_t *part = frame_ota_slot_partition(slot);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; off < part->size; off += FRAME_OTA_CHUNK_ERASE) {
        size_t n = part->size - off;
        if (n > FRAME_OTA_CHUNK_ERASE) {
            n = FRAME_OTA_CHUNK_ERASE;
        }
        esp_err_t err = esp_partition_erase_range(part, off, n);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    s_stats.erase_us += (uint32_t)(esp_timer_get_time() - t0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Image writer                                                          */
/* ------------------------------------------------------------------ */

struct frame_ota_writer {
    const esp_partition_t *part;
    esp_ota_handle_t      handle;
    int                   slot;
    size_t                size;
};

esp_err_t frame_ota_write_begin(int slot, size_t image_size, frame_ota_writer_t **out)
{
    if (out == NULL || s_slot_count == 0u || slot < 0 || slot >= s_slot_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    const esp_partition_t *part = frame_ota_slot_partition(slot);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    frame_ota_writer_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_ota_begin(part, image_size, &w->handle);
    if (err != ESP_OK) {
        free(w);
        return err;
    }

    w->part  = part;
    w->slot  = slot;
    w->size  = image_size;
    *out     = w;
    return ESP_OK;
}

esp_err_t frame_ota_write_chunk(frame_ota_writer_t *w, const void *data, size_t len)
{
    if (w == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_ota_write(w->handle, data, len);
    s_stats.write_us += (uint32_t)(esp_timer_get_time() - t0);
    if (err == ESP_OK) {
        s_stats.bytes_written += (uint32_t)len;
    }
    return err;
}

esp_err_t frame_ota_write_end(frame_ota_writer_t *w, const char *app_id, const char *version)
{
    if (w == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_ota_end(w->handle);
    if (err != ESP_OK) {
        /* esp_ota_end() already invalidated the handle. */
        frame_ota_release_slot(w->slot);
        free(w);
        return err;
    }

    err = frame_ota_commit_slot(w->slot, app_id, version);
    free(w);
    return err;
}

void frame_ota_write_abort(frame_ota_writer_t *w)
{
    if (w == NULL) {
        return;
    }
    (void)esp_ota_abort(w->handle);
    frame_ota_release_slot(w->slot);
    free(w);
}

/* ------------------------------------------------------------------ */
/* Boot selection                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t set_boot_partition(int slot)
{
    const esp_partition_t *part = (slot < 0)
        ? esp_ota_get_running_partition()
        : frame_ota_slot_partition(slot);

    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_ota_set_boot_partition(part);
}

static void save_handoff(const char *app_id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, KEY_HANDOFF, app_id);
    /* Remember which shell image (A or B) handed over, so the app can hand
     * control back to *that* image rather than guessing. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL && running->label != NULL) {
        nvs_set_str(h, KEY_SHELL, running->label);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_handoff(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_launched_app);
    if (nvs_get_str(h, KEY_HANDOFF, s_launched_app, &len) != ESP_OK) {
        s_launched_app[0] = '\0';
    }
    len = sizeof(s_shell_label);
    if (nvs_get_str(h, KEY_SHELL, s_shell_label, &len) != ESP_OK) {
        s_shell_label[0] = '\0';
    }
    nvs_close(h);
}

int frame_ota_set_boot_app(const char *app_id)
{
    if (app_id == NULL) {
        return -1;
    }
    const frame_app_state_t *app = frame_app_manager_find(app_id);
    if (app == NULL || app->kind != FRAME_APP_KIND_NATIVE) {
        return -1;
    }
    if (app->slot < 0 || app->slot >= s_slot_count) {
        return -1;
    }
    if (set_boot_partition(app->slot) != ESP_OK) {
        return -1;
    }
    save_handoff(app_id);
    return app->slot;
}

esp_err_t frame_ota_set_boot_shell(void)
{
    /* A running native app's own partition is not the shell. Prefer the shell
     * image that handed over (recorded in the handoff), else whichever shell
     * slot carries a valid image. */
    const esp_partition_t *shell = NULL;
    if (s_shell_label[0] != '\0') {
        shell = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY,
                                         s_shell_label);
    }
    if (shell == NULL) {
        shell = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                         ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    }
    if (shell == NULL) {
        shell = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                         ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    }
    if (shell == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_ota_set_boot_partition(shell);
}

bool frame_ota_launched_by_shell(char *app_id_out, size_t cap)
{
    if (app_id_out != NULL && cap > 0u) {
        strncpy(app_id_out, s_launched_app, cap - 1u);
        app_id_out[cap - 1u] = '\0';
    }
    return s_launched_app[0] != '\0';
}

void frame_ota_clear_handoff(void)
{
    s_launched_app[0] = '\0';
    s_shell_label[0]   = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, KEY_HANDOFF);
    nvs_erase_key(h, KEY_SHELL);
    nvs_commit(h);
    nvs_close(h);
}

void frame_ota_get_stats(frame_ota_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

const frame_slot_table_t *frame_ota_table(void)
{
    return &s_table;
}
