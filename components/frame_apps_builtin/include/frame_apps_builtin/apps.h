/*
 * frame_apps_builtin/apps.h — the apps that ship in every FrameOS image.
 *
 * Logs, Settings and the App Store are built in because a device without
 * them cannot be diagnosed, configured or extended. They are registered with
 * the app manager exactly like installed apps, so the launcher treats them
 * identically — the only difference is their kind.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_APPS_STORE_ID    "myframe.store"
#define FRAME_APPS_SETTINGS_ID "myframe.settings"
#define FRAME_APPS_LOGS_ID     "myframe.logs"

/* Registers the three built-ins with the app manager. Idempotent. */
esp_err_t frame_apps_register_builtins(void);

/*
 * Shows a built-in's page. Must run on the LVGL task. Returns ESP_ERR_NOT_FOUND
 * when `id` is not a built-in (the shell decides based on the app's kind).
 */
esp_err_t frame_apps_open(const char *id);

/* Individual pages (all LVGL-task only). */
void frame_app_logs_show(void);
void frame_app_settings_show(void);
void frame_app_store_show(void);

/* One-time setup: restore brightness/idle policy, subscribe to events. */
esp_err_t frame_app_settings_init(void);
esp_err_t frame_app_store_init(void);

/* Connects Wi-Fi with saved credentials, if any. Call after frame_net_init. */
void frame_app_settings_apply_wifi(void);

#ifdef __cplusplus
}
#endif
