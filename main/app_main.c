/*
 * app_main.c — the shell image's entry point.
 *
 * Boot order is a dependency order, not a preference (docs/ARCHITECTURE.md §4):
 *
 *   NVS → log → board → storage/registry → OTA table → app manager → built-ins
 *   → store (offline catalog) → display → UI → input → LVGL task → power
 *   → LAUNCHER (first pixels the user sees) → shell → wasm → network → API
 *   → watchdog → boot confirmed
 *
 * The launcher is shown before anything slow: Wi-Fi association, catalog
 * refresh and the HTTP server all start after the home screen is on the panel.
 */
#include "frame_apps_builtin/apps.h"
#include "frame_apps_builtin/launcher.h"
#include "frame_core/app_manager.h"
#include "frame_core/board_types.h"
#include "frame_core/watchdog.h"
#include "frame_display/display.h"
#include "frame_input/input.h"
#include "frame_log/log.h"
#include "frame_net/net.h"
#include "frame_ota/ota.h"
#include "frame_shell/shell.h"
#include "frame_storage/storage.h"
#include "frame_store/store.h"
#include "frame_ui/ui.h"
#include "frame_wasm/wasm.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>

static const char *TAG = "boot";

#ifndef CONFIG_FRAME_LOG_RING_KB
#define CONFIG_FRAME_LOG_RING_KB 256
#endif
#ifndef CONFIG_FRAME_HTTP_API_ENABLE
#define CONFIG_FRAME_HTTP_API_ENABLE 1
#endif
#ifndef CONFIG_FRAME_BOOT_CONFIRM_MS
#define CONFIG_FRAME_BOOT_CONFIRM_MS 2500
#endif

/* ------------------------------------------------------------------ */
/* Boot confirmation                                                    */
/* ------------------------------------------------------------------ */

static void boot_confirm_cb(void *arg)
{
    (void)arg;
    /* Reaching here means the LVGL task has been feeding the watchdog for
     * CONFIG_FRAME_BOOT_CONFIRM_MS: the image is healthy. */
    frame_watchdog_mark_boot_ok();
    (void)frame_ota_confirm_boot();
    ESP_LOGI(TAG, "boot complete: %" PRIu32 " ms, %u KB internal heap free",
             (uint32_t)(esp_timer_get_time() / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u));
}

static void schedule_boot_confirm(void)
{
    const esp_timer_create_args_t args = {
        .callback = boot_confirm_cb,
        .name = "frame_boot_confirm",
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        (void)esp_timer_start_once(t, (uint64_t)CONFIG_FRAME_BOOT_CONFIRM_MS * 1000u);
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    const int64_t t0 = esp_timer_get_time();
    const frame_board_t *board = frame_board();
    ESP_LOGI(TAG, "FrameOS boot on %s rev %s", board->id, board->revision);

    /* NVS first: settings, registry, slot table and the watchdog's failure
     * counter all live there, and the watchdog must read its counter before
     * anything can reset it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing (%s)", esp_err_to_name(err));
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(err));
    }

    /* Liveness before anything that might hang. */
    (void)frame_watchdog_init();
    if (frame_watchdog_safe_mode()) {
        ESP_LOGE(TAG, "safe mode: skipping managed apps and native slots");
    }

    frame_log_init((size_t)CONFIG_FRAME_LOG_RING_KB * 1024u);
    FRAME_LOGI(TAG, "Frame SDK " FRAME_SDK_VERSION ", ABI %u", (unsigned)FRAME_ABI_VERSION);

    /* Hardware. */
    err = board->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    (void)frame_storage_init();
    (void)frame_storage_mount_fs();

    (void)frame_ota_init();
    (void)frame_app_manager_init();
    (void)frame_apps_register_builtins();
    const size_t restored = frame_registry_load_all();
    ESP_LOGI(TAG, "registry: %u app(s) restored", (unsigned)restored);

    (void)frame_store_init();
    (void)frame_store_reconcile();

    /* Display, UI, input — in this order, because frame_display_start() hands
     * LVGL to its own task and nothing may create objects after that. */
    err = frame_display_init(board->display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    }
    err = frame_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui init failed: %s", esp_err_to_name(err));
    }
    if (board->touch != NULL) {
        (void)frame_input_init(board->touch);
    }
    (void)frame_display_start();
    (void)frame_power_init(NULL);

    (void)frame_app_settings_init();
    (void)frame_app_store_init();
    (void)frame_launcher_init();

    /* First pixels: the launcher, before Wi-Fi or the catalog. */
    frame_ui_home();
    ESP_LOGI(TAG, "launcher shown after %" PRIi32 " ms", (int32_t)((esp_timer_get_time() - t0) / 1000));

    (void)frame_shell_init();

    if (!frame_watchdog_safe_mode()) {
        (void)frame_wasm_init();
    }

    (void)frame_net_init();
    frame_app_settings_apply_wifi();

    if (CONFIG_FRAME_HTTP_API_ENABLE) {
        (void)frame_store_api_start();
    }

    schedule_boot_confirm();
}
