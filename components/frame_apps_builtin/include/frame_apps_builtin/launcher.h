/*
 * frame_apps_builtin/launcher.h — the home screen.
 *
 * The launcher is the first thing the device shows. Every installed app is a
 * tile here and the store is a pinned tile, so nothing on the device is more
 * than one tap from home.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_LAUNCHER_APP_ID "myframe.launcher"

/* Registers the launcher as the shell's "home". Call once at boot. */
esp_err_t frame_launcher_init(void);

/* Builds the home page. Must run on the LVGL task (frame_ui_home() ensures it). */
void frame_launcher_show(void);

/* Rebuilds the tiles from the app manager. Dispatch-safe. */
void frame_launcher_refresh(void);

/* Called by the shell when another app takes over the content area. */
void frame_launcher_hide(void);

bool frame_launcher_visible(void);

#ifdef __cplusplus
}
#endif
