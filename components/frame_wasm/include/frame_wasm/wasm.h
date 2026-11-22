/*
 * frame_wasm/wasm.h — managed apps: wasm3 in-process runtime.
 *
 * A managed app is a .wasm module installed under /apps/<id>/app.wasm. It is
 * loaded, linked against a small host API and run on the wasm task (core 1,
 * priority 3, below LVGL). Switching between two managed apps is a release +
 * load, not a reboot.
 *
 * The ABI the module sees is deliberately tiny and declarative (see
 * docs/APP_SDK.md §8): it never receives an LVGL pointer. It emits a retained
 * UI tree through frame_ui_begin/node/set_text/set_style/end, and the shell
 * diffs that into LVGL on the LVGL task. Button presses come back through the
 * exported frame_app_event(). frame_app_tick() is called ~10x/s if exported.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Events delivered to the module's frame_app_event(type, a, b) export. */
#define FRAME_WASM_EVT_CLICK  1
#define FRAME_WASM_EVT_TICK   2
#define FRAME_WASM_EVT_PAUSE  3
#define FRAME_WASM_EVT_RESUME 4
#define FRAME_WASM_EVT_STOP   5

/* Node types for frame_ui_node(). Must match the SDK docs; never renumber. */
#define FRAME_WASM_NODE_ROOT      0
#define FRAME_WASM_NODE_CONTAINER 1
#define FRAME_WASM_NODE_LABEL     2
#define FRAME_WASM_NODE_BUTTON    3
#define FRAME_WASM_NODE_ROW       4
#define FRAME_WASM_NODE_PROGRESS  5

esp_err_t frame_wasm_init(void);

/* Loads and starts /apps/<id>/app.wasm. Returns once the module's
 * frame_app_main() has produced its first UI tree (or failed). Blocking: call
 * from the shell's launch path, which already runs on a worker. */
esp_err_t frame_wasm_launch(const char *app_id);

/* Stops the running module and frees its runtime. Blocks until released
 * (bounded). Call from the shell/worker, never from the LVGL task. */
esp_err_t frame_wasm_stop(void);

/* Asynchronous stop for UI callbacks: posts the request and returns at once.
 * The wasm task returns to the launcher when it has released the module. */
void frame_wasm_request_stop(void);

/* Queues an event for the running module. Safe from any task. */
esp_err_t frame_wasm_post_event(int32_t type, int32_t a, int32_t b);

bool        frame_wasm_running(void);
const char *frame_wasm_current_app(void);

/* Diagnostics for Settings → Developer. */
void frame_wasm_get_memory(size_t *linear_bytes, size_t *stack_bytes);

#ifdef __cplusplus
}
#endif
