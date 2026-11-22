/*
 * frame_ui/ui.h — the shell: theme, screen scaffolding, cross-thread dispatch.
 *
 * Every pixel is produced on the LVGL task (core 1). Anything that runs on
 * another core — the store downloader, the WASM runtime, the network stack —
 * reaches the UI only through frame_ui_dispatch(), which queues a closure for
 * the LVGL task. There is no other door in, which is what keeps LVGL's
 * single-threaded contract intact without sprinkling locks everywhere.
 *
 * Layout (800x480 on the first board, scaled by the board's real size):
 *
 *   ┌──────────────────────────────────────────────┐
 *   │ top bar  44 px: title · wifi · updates · clock│
 *   ├──────────────────────────────────────────────┤
 *   │ content  h-72 px: launcher / app UI           │
 *   ├──────────────────────────────────────────────┤
 *   │ footer   28 px: hint text / launcher dots     │
 *   └──────────────────────────────────────────────┘
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_UI_TOPBAR_H  44u
#define FRAME_UI_FOOTER_H  28u
#define FRAME_UI_MAX_TITLE 48u

typedef struct {
    uint32_t bg;        /* screen background                 */
    uint32_t surface;   /* top bar, footer                   */
    uint32_t card;      /* tiles, list rows                  */
    uint32_t card_hi;   /* pressed / highlighted             */
    uint32_t accent;    /* primary — store tile, selection   */
    uint32_t accent2;   /* secondary — builtin icons         */
    uint32_t text;
    uint32_t text_dim;
    uint32_t danger;
    uint32_t ok;
} frame_ui_theme_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t content_h;
} frame_ui_metrics_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

esp_err_t frame_ui_init(void);

/* The root screen; also the parent of the top bar and footer. */
lv_obj_t *frame_ui_screen(void);

/* The container an app draws into. Lives below the top bar. */
lv_obj_t *frame_ui_content(void);

const frame_ui_theme_t *frame_ui_theme(void);
void frame_ui_metrics(frame_ui_metrics_t *out);

/* ------------------------------------------------------------------ */
/* Navigation                                                           */
/* ------------------------------------------------------------------ */

/*
 * Clears the content area and returns a fresh container of its full size for
 * the caller to populate. The previous content is deleted.
 *
 * LVGL task only. Everything under "Navigation" and "Widgets" except the
 * functions marked "safe from any task" is LVGL-task only: reach it from
 * another task through frame_ui_dispatch().
 */
lv_obj_t *frame_ui_page(const char *title);

/*
 * Registers what "home" means. The launcher installs itself here so that the
 * shell never has to depend on any particular app.
 */
void frame_ui_set_home(frame_ui_fn_t show, void *arg);

/* Pops back to the launcher (or whatever is registered as home). Any task. */
void frame_ui_home(void);

/* Shows a back arrow in the top bar that calls fn(arg) when clicked. */
void frame_ui_set_back(void (*fn)(void *), void *arg);
void frame_ui_clear_back(void);

void frame_ui_set_title(const char *title);   /* any task */

/* Status indicators. */
void frame_ui_status_wifi(bool up, int8_t rssi);
void frame_ui_status_updates(uint32_t count);
void frame_ui_status_progress(int percent);   /* -1 hides */

void frame_ui_footer(const char *text);       /* any task */

/* ------------------------------------------------------------------ */
/* Cross-thread dispatch                                                */
/* ------------------------------------------------------------------ */

typedef void (*frame_ui_fn_t)(void *arg);

/*
 * Runs fn(arg) on the LVGL task. Safe from any task; never from an ISR.
 * Returns ESP_ERR_NO_MEM if the dispatch queue is full (the LVGL task is
 * stalled) — the caller must handle that rather than block.
 */
esp_err_t frame_ui_dispatch(frame_ui_fn_t fn, void *arg);

/*
 * Runs fn(arg) on the LVGL task and waits for it to finish. Used by code that
 * needs an answer (the WASM runtime reading a textarea, a test hook).
 */
esp_err_t frame_ui_dispatch_wait(frame_ui_fn_t fn, void *arg, uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Widgets                                                              */
/* ------------------------------------------------------------------ */

/* Transient message at the bottom of the content area. */
void frame_ui_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Modal progress card. percent < 0 closes it. */
void frame_ui_progress(const char *title, int percent, const char *detail);
void frame_ui_progress_close(void);

typedef void (*frame_ui_confirm_cb_t)(bool accepted, void *user);
void frame_ui_confirm(const char *title, const char *body,
                      const char *accept_label, const char *cancel_label,
                      frame_ui_confirm_cb_t cb, void *user);

/* Styling helpers shared by the builtin apps so they look like one system. */
lv_obj_t *frame_ui_card(lv_obj_t *parent);
lv_obj_t *frame_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                         uint32_t color);
lv_obj_t *frame_ui_button(lv_obj_t *parent, const char *text, uint32_t bg,
                          lv_event_cb_t cb, void *user);
lv_obj_t *frame_ui_row(lv_obj_t *parent, uint16_t height);
void       frame_ui_spacer(lv_obj_t *parent);
lv_obj_t  *frame_ui_section(lv_obj_t *parent, const char *title);
lv_obj_t  *frame_ui_list(lv_obj_t *parent);

/* Standard click handler that navigates home. */
void frame_ui_back_to_home(lv_obj_t *btn);

#ifdef __cplusplus
}
#endif
