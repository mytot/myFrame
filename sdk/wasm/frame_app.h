/*
 * frame_app.h — the managed (WASM) App SDK.
 *
 * A managed app is a single .wasm module. It never sees LVGL or a peripheral;
 * it describes a UI as a tree of nodes and the shell renders it. That is what
 * makes it safe to hot-load in ~10 ms with no reboot.
 *
 * Build (see examples/wasm_hello and tools/build_app.py):
 *
 *   clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry \
 *         -Wl,--export=frame_app_main -Wl,--export=frame_app_event \
 *         -Wl,--export=frame_app_tick -Wl,--import-undefined \
 *         -Wl,--initial-memory=131072 -Wl,--max-memory=524288 \
 *         -Wl,--strip-all -o app.wasm app.c
 *
 * Exports (the module side):
 *   void frame_app_main(void)            required; build the first UI tree
 *   void frame_app_event(int type, int a, int b)   optional
 *   void frame_app_tick(void)            optional; called ~10x/s
 *
 * Limits enforced by the host: 64 nodes, 160 operations per frame_ui_begin/
 * end batch, strings <= 48 bytes, linear memory <= 512 KB.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_EXPORT(name) __attribute__((export_name(name)))
#define FRAME_IMPORT(name) __attribute__((import_module("env"), import_name(name)))

/* Events delivered to frame_app_event(). */
#define FRAME_EVT_CLICK   1   /* a = node handle of the button        */
#define FRAME_EVT_TICK    2
#define FRAME_EVT_PAUSE   3   /* screen dimmed/off: stop animating    */
#define FRAME_EVT_RESUME  4
#define FRAME_EVT_STOP    5   /* the app is about to be unloaded      */

/* Node types for frame_ui_node(). Never renumber. */
#define FRAME_NODE_ROOT      0
#define FRAME_NODE_CONTAINER 1
#define FRAME_NODE_LABEL     2
#define FRAME_NODE_BUTTON    3
#define FRAME_NODE_ROW       4
#define FRAME_NODE_PROGRESS  5

/* A handle to a node the host created. 0 is the page root. */
typedef int32_t frame_node_t;

/* ------------------------------------------------------------------ */
/* UI                                                                   */
/* ------------------------------------------------------------------ */

void frame_ui_begin(void) FRAME_IMPORT("frame_ui_begin");

/*
 * Creates a node under `parent` (0 = page root) and returns its handle.
 * `props` is an optional "key=value;key=value" string applied immediately:
 *
 *   text_font=14|20|28|display    text_color=#RRGGBB    bg=#RRGGBB
 *   pad=<px>   width=fill|<px>    height=fill|<px>      radius=<px>
 *   align=center|left|right|top|bottom                  text_align=center|left|right
 *   flex=row|column               scroll=on|off
 */
int32_t frame_ui_node_raw(frame_node_t parent, int32_t type, const char *id,
                          const char *props, int32_t props_len) FRAME_IMPORT("frame_ui_node");

static inline frame_node_t frame_ui_node(frame_node_t parent, int32_t type,
                                         const char *props)
{
    int32_t n = 0;
    if (props != NULL) {
        while (props[n] != '\0') {
            n++;
        }
    }
    return frame_ui_node_raw(parent, type, (const char *)0, props, n);
}

static inline frame_node_t frame_ui_node_with_id(frame_node_t parent, int32_t type,
                                                 const char *id, const char *props)
{
    int32_t n = 0;
    if (props != NULL) {
        while (props[n] != '\0') {
            n++;
        }
    }
    return frame_ui_node_raw(parent, type, id, props, n);
}

void frame_ui_set_text(frame_node_t node, const char *text) FRAME_IMPORT("frame_ui_set_text");
void frame_ui_set_style(frame_node_t node, const char *prop, const char *value)
    FRAME_IMPORT("frame_ui_set_style");
void frame_ui_end(void) FRAME_IMPORT("frame_ui_end");
void frame_ui_title(const char *title) FRAME_IMPORT("frame_ui_title");

/* Text shorter than this is guaranteed to arrive intact. */
#define FRAME_TEXT_MAX 48

/* ------------------------------------------------------------------ */
/* Services                                                             */
/* ------------------------------------------------------------------ */

void frame_log_i(const char *tag, const char *msg) FRAME_IMPORT("frame_log_i");
void frame_log_w(const char *tag, const char *msg) FRAME_IMPORT("frame_log_w");

/* Milliseconds since boot (wraps at 2^32, ~49 days). */
uint32_t frame_now_ms(void) FRAME_IMPORT("frame_now_ms");

/*
 * Key/value storage scoped to this app (host prefixes keys with the app id).
 * frame_kv_get copies at most cap-1 bytes plus a NUL into out and returns the
 * length written, or 0 when the key does not exist. Values are <= 127 bytes.
 */
int32_t frame_kv_get(const char *key, char *out, int32_t cap) FRAME_IMPORT("frame_kv_get");
int32_t frame_kv_set(const char *key, const char *value) FRAME_IMPORT("frame_kv_set");

/* Reserved for the async HTTP API; currently returns -1 (unsupported). */
int32_t frame_http_get(const char *url, int32_t handle) FRAME_IMPORT("frame_http_get");

/* Asks the host to unload this app and return to the launcher. */
void frame_app_exit(void) FRAME_IMPORT("frame_app_exit");

#ifdef __cplusplus
}
#endif
