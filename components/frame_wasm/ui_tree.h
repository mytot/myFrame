/* frame_wasm internal: retained UI tree bridge between the wasm task and LVGL. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define UI_OP_NODE   1
#define UI_OP_TEXT   2
#define UI_OP_STYLE  3

/* wasm task */
void ui_tree_reset(void);
void ui_tree_record_node(int32_t node, int32_t parent, int32_t type, const char *id);
void ui_tree_record_text(int32_t node, const char *text);
void ui_tree_record_style(int32_t node, const char *prop, const char *value);
void ui_tree_commit(void);

/* Allocates the op buffer (PSRAM). Call once before the first launch. */
esp_err_t ui_tree_init(void);

/* Either task (hops to the LVGL task when needed). `on_back` runs on the
 * LVGL task when the top-bar back arrow is pressed. */
void ui_tree_open(const char *title, void (*on_back)(void *), void *arg);
void ui_tree_close(void);
