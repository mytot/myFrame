/*
 * frame_shell/shell.h — the shell: turns "launch app X" into the right
 * action for that app's kind, off the LVGL task.
 *
 *   BUILTIN  -> show its page (frame_apps_builtin), on the LVGL task
 *   NATIVE   -> point the boot partition at its slot and restart (~300-500 ms)
 *   MANAGED  -> wasm3 hot-load on the wasm task (~5-20 ms, no reboot)
 *
 * Launches are queued to a small worker so the LVGL task never blocks on a
 * flash write, a wasm load or a restart.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_SHELL_APP_ID "myframe.shell"

/* Registers built-in apps, subscribes to FRAME_EVT_APP_LAUNCH and starts the
 * launch worker. Call after the UI, store and wasm runtime are initialised. */
esp_err_t frame_shell_init(void);

/* Queues a launch by app id. Safe from any task, including the LVGL task. */
esp_err_t frame_shell_open(const char *app_id);

/* Back to the launcher (stops a running managed app). Dispatch-safe. */
void frame_shell_home(void);

#ifdef __cplusplus
}
#endif
