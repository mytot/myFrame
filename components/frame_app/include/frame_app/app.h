/*
 * frame_app/app.h — the native App SDK.
 *
 * A native app is a complete ESP-IDF image built from the same components as
 * the shell, plus the app's own code, with this component supplying app_main().
 * It lives in an OTA slot and is launched by a reboot-switch (the shell points
 * the bootloader at it and restarts), which is why it can use every peripheral
 * directly and why switching to it costs ~300-500 ms.
 *
 * The app declares one descriptor:
 *
 *   static esp_err_t on_start(frame_ctx_t *ctx) {
 *       lv_obj_t *page = frame_ui_page("Counter");
 *       ...
 *       return ESP_OK;
 *   }
 *   FRAME_APP_DEFINE("example.counter", "Counter", "1.2.0", NULL,
 *                    .on_start = on_start, .on_event = on_event);
 *
 * Lifecycle callbacks run on the LVGL task, so they may touch LVGL directly.
 * Anything slow belongs on its own task and reaches the UI through
 * frame_ui_dispatch(), exactly like the shell's store task does.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "frame_core/app.h"
#include "frame_core/event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this SDK header set; must match FRAME_SDK_VERSION in the shell. */
#define FRAME_APP_SDK_VERSION FRAME_SDK_VERSION

/* ------------------------------------------------------------------ */
/* Descriptor                                                           */
/* ------------------------------------------------------------------ */

/*
 * Declares the app. Exactly one per image. `permissions` is a NULL-terminated
 * list of strings copied into the manifest by tools/pack.py when the package
 * is built; the device enforces them at install time.
 */
#define FRAME_APP_DEFINE(ID, NAME, VERSION, PERMS, ...)                              \
    __attribute__((used)) const frame_app_descriptor_t frame_app_descriptor = {      \
        .abi_version  = FRAME_ABI_VERSION,                                           \
        .id          = (ID),                                                         \
        .name        = (NAME),                                                       \
        .version     = (VERSION),                                                    \
        .permissions = (PERMS),                                                      \
        __VA_ARGS__                                                                  \
    }

/* Defined by FRAME_APP_DEFINE; read by the runtime. */
extern const frame_app_descriptor_t frame_app_descriptor;

/* ------------------------------------------------------------------ */
/* Runtime API (callable from the app's callbacks)                      */
/* ------------------------------------------------------------------ */

/* The context handed to every callback; valid for the app's lifetime. */
const frame_ctx_t *frame_app_ctx(void);

/*
 * Hands control back to the shell: on_stop/on_destroy run, the bootloader is
 * pointed at the shell image, and the device restarts. The healthy way for an
 * app to end; an app that simply returns from on_start keeps running.
 */
void frame_app_exit(void);

/*
 * Confirms the image is healthy, cancelling the bootloader's pending
 * rollback. Called automatically a moment after on_start() returns ESP_OK;
 * call it earlier if the app wants a shorter verification window, or after a
 * critical self-test.
 */
void frame_app_confirm(void);

/* ------------------------------------------------------------------ */
/* Services                                                             */
/* ------------------------------------------------------------------ */

/*
 * The shell's components are linked in, so the app uses them directly:
 *
 *   UI        frame_ui/ui.h        (frame_ui_page, frame_ui_button, ...)
 *   Logging   frame_log/log.h      (FRAME_LOGI / FRAME_LOGE)
 *   Settings  frame_storage/storage.h (frame_settings_*; keys are app-scoped
 *                                      when the manifest declares "kv")
 *   Network   frame_net/net.h      (only with the "net" permission)
 *   Events    frame_core/event.h   (delivered on the LVGL task)
 *   Board     frame_core/board_types.h (frame_board(): backlight, sleep)
 *
 * frame_ctx_t fields (kv/fs/log/ui/net/audio) are reserved for the managed
 * runtime's per-app sandboxes and are NULL here.
 */

#ifdef __cplusplus
}
#endif
