/*
 * app_runtime.c — app_main() for a native app image.
 *
 * Mirrors the shell's boot sequence with the shell-only parts removed (no
 * launcher, store, catalog or WASM). The descriptor is found through the
 * `frame_app_descriptor` symbol that FRAME_APP_DEFINE emits.
 */
#include "frame_app/app.h"

#include "frame_core/app_manager.h"
#include "frame_core/board_types.h"
#include "frame_core/power.h"
#include "frame_core/watchdog.h"
#include "frame_display/display.h"
#include "frame_input/input.h"
#include "frame_log/log.h"
#include "frame_ota/ota.h"
#include "frame_storage/storage.h"
#include "frame_ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "app";

#ifndef CONFIG_FRAME_LOG_RING_KB
#define CONFIG_FRAME_LOG_RING_KB 128
#endif

static frame_ctx_t     s_ctx;
static bool            s_running;
static bool            s_confirmed;

const frame_ctx_t *frame_app_ctx(void)
{
    return &s_ctx;
}

void frame_app_confirm(void)
{
    if (s_confirmed) {
        return;
    }
    s_confirmed = true;
    (void)frame_ota_confirm_boot();
    frame_app_manager_mark_healthy(s_ctx.app_id);
    FRAME_LOGI(TAG, "%s confirmed healthy", s_ctx.app_id);
}

void frame_app_exit(void)
{
    FRAME_LOGI(TAG, "%s exiting to shell", s_ctx.app_id);
    if (frame_app_descriptor.on_stop != NULL && s_running) {
        frame_app_descriptor.on_stop(&s_ctx);
    }
    if (frame_app_descriptor.on_destroy != NULL) {
        frame_app_descriptor.on_destroy(&s_ctx);
    }
    frame_app_manager_mark_launched(s_ctx.app_id, (uint32_t)(esp_timer_get_time() / 1000));

    /* Hand the bootloader back to the shell image that launched us. If this
     * image was flashed directly there may be no shell; in that case the
     * restart simply comes back here. */
    (void)frame_ota_set_boot_shell();
    frame_ota_clear_handoff();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

static void on_system_event(const frame_event_t *evt, void *user)
{
    (void)user;
    if (!s_running) {
        return;
    }
    /* Power events matter to everyone: an app that animates should stop when
     * the panel is off and resume when the user touches it. */
    if (evt->type == FRAME_EVT_POWER_IDLE || evt->type == FRAME_EVT_POWER_OFF) {
        if (frame_app_descriptor.on_pause != NULL) {
            (void)frame_app_descriptor.on_pause(&s_ctx);
        }
        return;
    }
    if (evt->type == FRAME_EVT_POWER_WAKE) {
        if (frame_app_descriptor.on_resume != NULL) {
            (void)frame_app_descriptor.on_resume(&s_ctx);
        }
        return;
    }
    if (frame_app_descriptor.on_event != NULL) {
        frame_app_descriptor.on_event(&s_ctx, evt);
    }
}

static void confirm_cb(void *arg)
{
    (void)arg;
    frame_app_confirm();
    frame_watchdog_mark_boot_ok();
}

void app_main(void)
{
    const int64_t t0 = esp_timer_get_time();
    const frame_app_descriptor_t *d = &frame_app_descriptor;

    if (d->abi_version != FRAME_ABI_VERSION) {
        ESP_LOGE(TAG, "descriptor ABI %u != runtime ABI %u", (unsigned)d->abi_version,
                 (unsigned)FRAME_ABI_VERSION);
        esp_restart();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }
    (void)frame_watchdog_init();
    frame_log_init((size_t)CONFIG_FRAME_LOG_RING_KB * 1024u);
    ESP_LOGI(TAG, "starting %s %s (launched by shell: %s)", d->id, d->version,
             frame_ota_launched_by_shell(NULL, 0) ? "yes" : "no");

    (void)frame_ota_init();
    (void)frame_storage_init();
    (void)frame_storage_mount_fs();
    (void)frame_app_manager_init();

    const frame_board_t *board = frame_board();
    (void)board->init();
    (void)frame_display_init(board->display);
    (void)frame_ui_init();
    if (board->touch != NULL) {
        (void)frame_input_init(board->touch);
    }
    (void)frame_display_start();
    (void)frame_power_init(NULL);

    /* The app manager keeps the "last launched" bookkeeping that the shell's
     * launcher reads, so record this app in it. */
    frame_app_state_t st;
    memset(&st, 0, sizeof(st));
    snprintf(st.id, sizeof(st.id), "%s", d->id);
    snprintf(st.name, sizeof(st.name), "%s", d->name);
    snprintf(st.version, sizeof(st.version), "%s", d->version);
    st.kind    = FRAME_APP_KIND_NATIVE;
    st.slot    = FRAME_APP_NO_SLOT;
    st.healthy = true;
    (void)frame_app_manager_upsert(&st);

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.app_id = d->id;
    s_ctx.state  = NULL;

    if (d->on_create != NULL) {
        err = d->on_create(&s_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "on_create failed: %s", esp_err_to_name(err));
            frame_app_exit();
            return;
        }
    }
    if (d->on_start != NULL) {
        err = d->on_start(&s_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "on_start failed: %s", esp_err_to_name(err));
            frame_app_exit();
            return;
        }
    }
    s_running = true;
    (void)frame_event_subscribe(FRAME_EVT_NONE, on_system_event, NULL);

    /* A native image boots in PENDING_VERIFY when it was written by the store.
     * Give the first frames a moment, then confirm so a crash here rolls back
     * to the shell instead of boot-looping. */
    const esp_timer_create_args_t args = { .callback = confirm_cb, .name = "app_confirm" };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        (void)esp_timer_start_once(t, 1500ull * 1000ull);
    }

    frame_app_manager_mark_launched(d->id, (uint32_t)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "%s ready in %" PRIi32 " ms, %u KB internal heap free", d->id,
             (int32_t)((esp_timer_get_time() - t0) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u));
}
