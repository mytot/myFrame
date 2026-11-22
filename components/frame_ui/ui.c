#include "frame_ui/ui.h"

#include "frame_core/event.h"
#include "frame_display/display.h"
#include "frame_log/log.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui";

/* ------------------------------------------------------------------ */
/* Theme                                                               */
/* ------------------------------------------------------------------ */

static const frame_ui_theme_t s_theme = {
    .bg       = 0x0B0F14,
    .surface  = 0x11161D,
    .card     = 0x1A212B,
    .card_hi  = 0x232D3A,
    .accent   = 0x2DD4BF,
    .accent2  = 0x60A5FA,
    .text     = 0xE6EDF3,
    .text_dim = 0x8B98A5,
    .danger   = 0xF87171,
    .ok       = 0x34D399,
};

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    frame_ui_fn_t     fn;
    void             *arg;
    SemaphoreHandle_t done;   /* set for dispatch_wait(), NULL otherwise */
} dispatch_t;

#define DISPATCH_LEN 32u
#define TOAST_MAX    160u

static QueueHandle_t  s_dispatch;
static lv_timer_t    *s_dispatch_timer;
static lv_timer_t    *s_clock_timer;

static lv_obj_t *s_screen;
static lv_obj_t *s_topbar;
static lv_obj_t *s_content;
static lv_obj_t *s_footer;

static lv_obj_t *s_back_btn;
static lv_obj_t *s_title;
static lv_obj_t *s_wifi;
static lv_obj_t *s_upd_pill;
static lv_obj_t *s_upd_label;
static lv_obj_t *s_prog_mini;
static lv_obj_t *s_clock;
static lv_obj_t *s_footer_label;

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_toast_label;

static lv_obj_t *s_prog_overlay;
static lv_obj_t *s_prog_title;
static lv_obj_t *s_prog_bar;
static lv_obj_t *s_prog_detail;

static frame_ui_metrics_t s_metrics;
static bool               s_ready;
/* Set the first time the dispatch timer runs, which is by definition on the
 * LVGL task. Only that task may touch LVGL objects; a core id is not a
 * substitute (the wasm task also lives on core 1). */
static TaskHandle_t       s_lvgl_task;

static frame_ui_fn_t s_home_fn;
static void         *s_home_arg;
static frame_ui_fn_t s_back_fn;
static void         *s_back_arg;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void style_panel(lv_obj_t *o, uint32_t bg)
{
    lv_obj_set_style_bg_color(o, c(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, 0, 0);
    lv_obj_set_style_pad_column(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t *label_at(lv_obj_t *parent, const char *text, const lv_font_t *font,
                          uint32_t color, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c(color), 0);
    lv_obj_align(l, align, x, y);
    return l;
}

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur == NULL || strcmp(cur, text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void wake_lvgl(void)
{
    /* frame_event_poll() is what the LVGL task sleeps on; an event wakes it
     * immediately instead of waiting for the next 16-100 ms tick. */
    (void)frame_event_post_simple(FRAME_EVT_UI_DISPATCH, 0, 0);
}

esp_err_t frame_ui_dispatch(frame_ui_fn_t fn, void *arg)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dispatch == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    dispatch_t d = { .fn = fn, .arg = arg, .done = NULL };
    if (xQueueSend(s_dispatch, &d, 0) != pdTRUE) {
        FRAME_LOGW(TAG, "ui dispatch queue full; dropping call");
        return ESP_ERR_NO_MEM;
    }
    wake_lvgl();
    return ESP_OK;
}

static bool on_lvgl_task(void)
{
    return s_ready && s_task_is_lvgl();
}

static bool s_task_is_lvgl(void);

static bool s_task_is_lvgl(void)
{
    if (s_lvgl_task == NULL) {
        return false;
    }
    return xTaskGetCurrentTaskHandle() == s_lvgl_task;
}

static void dispatch_drain(lv_timer_t *t)
{
    (void)t;
    if (s_lvgl_task == NULL) {
        s_lvgl_task = xTaskGetCurrentTaskHandle();
    }
    dispatch_t d;
    /* Bound the work per frame so a flood of dispatches cannot starve rendering. */
    for (unsigned i = 0; i < 8u; i++) {
        if (xQueueReceive(s_dispatch, &d, 0) != pdTRUE) {
            break;
        }
        d.fn(d.arg);
        if (d.done != NULL) {
            xSemaphoreGive(d.done);
        }
    }
}

typedef struct {
    dispatch_t d;
} wait_item_t;

static void dispatch_wait_runner(void *arg)
{
    wait_item_t *w = (wait_item_t *)arg;
    w->d.fn(w->d.arg);
    SemaphoreHandle_t done = w->d.done;
    free(w);
    if (done != NULL) {
        xSemaphoreGive(done);
    }
}

esp_err_t frame_ui_dispatch_wait(frame_ui_fn_t fn, void *arg, uint32_t timeout_ms)
{
    if (fn == NULL || s_dispatch == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wait_item_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        vSemaphoreDelete(done);
        return ESP_ERR_NO_MEM;
    }
    w->d.fn   = fn;
    w->d.arg  = arg;
    w->d.done = done;

    dispatch_t d = { .fn = dispatch_wait_runner, .arg = w, .done = NULL };
    if (xQueueSend(s_dispatch, &d, 0) != pdTRUE) {
        free(w);
        vSemaphoreDelete(done);
        return ESP_ERR_NO_MEM;
    }
    wake_lvgl();

    if (xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* The call is still queued and will run; the caller just stops waiting.
         * The runner owns the semaphore from here on. */
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(done);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Top bar / footer                                                    */
/* ------------------------------------------------------------------ */

static void back_clicked(lv_event_t *e)
{
    (void)e;
    if (s_back_fn != NULL) {
        s_back_fn(s_back_arg);
    }
}

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    if (s_clock == NULL) {
        return;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        /* No NTP yet. Show the product name rather than 1970. */
        set_text_if_changed(s_clock, "");
        return;
    }
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    set_text_if_changed(s_clock, buf);
}

static void place_title(void)
{
    if (s_title == NULL) {
        return;
    }
    const bool back = (s_back_btn != NULL && lv_obj_has_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN) == false);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, back ? 56 : 16, 0);
}

static void title_apply(void *arg)
{
    char *text = (char *)arg;
    if (s_title != NULL) {
        set_text_if_changed(s_title, text);
        place_title();
    }
    free(text);
}

/* Safe from any task. */
void frame_ui_set_title(const char *title)
{
    if (s_title == NULL || title == NULL) {
        return;
    }
    if (on_lvgl_task()) {
        set_text_if_changed(s_title, title);
        return;
    }
    char *copy = malloc(strlen(title) + 1u);
    if (copy == NULL) {
        return;
    }
    strcpy(copy, title);
    if (frame_ui_dispatch(title_apply, copy) != ESP_OK) {
        free(copy);
    }
}

void frame_ui_set_back(void (*fn)(void *), void *arg)
{
    s_back_fn  = fn;
    s_back_arg = arg;
    if (s_back_btn != NULL) {
        if (fn != NULL) {
            lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
        }
        place_title();
    }
}

void frame_ui_clear_back(void)
{
    frame_ui_set_back(NULL, NULL);
}

void frame_ui_status_wifi(bool up, int8_t rssi)
{
    if (s_wifi == NULL) {
        return;
    }
    if (!up) {
        set_text_if_changed(s_wifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(s_wifi, c(s_theme.text_dim), 0);
        lv_obj_set_style_text_opa(s_wifi, LV_OPA_40, 0);
        return;
    }
    /* Signal strength is reflected in opacity: solid above -60 dBm, dimmer
     * as the link degrades. RSSI is still useful to callers for diagnostics. */
    set_text_if_changed(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(s_wifi, (rssi > -70) ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_obj_set_style_text_color(s_wifi, c(s_theme.ok), 0);
    lv_obj_set_style_text_opa(s_wifi, LV_OPA_COVER, 0);
}

void frame_ui_status_updates(uint32_t count)
{
    if (s_upd_pill == NULL) {
        return;
    }
    if (count == 0u) {
        lv_obj_add_flag(s_upd_pill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP " %u", (unsigned)count);
    set_text_if_changed(s_upd_label, buf);
    lv_obj_clear_flag(s_upd_pill, LV_OBJ_FLAG_HIDDEN);
}

void frame_ui_status_progress(int percent)
{
    if (s_prog_mini == NULL) {
        return;
    }
    if (percent < 0) {
        lv_obj_add_flag(s_prog_mini, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_bar_set_value(s_prog_mini, percent, LV_ANIM_OFF);
    lv_obj_clear_flag(s_prog_mini, LV_OBJ_FLAG_HIDDEN);
}

static void footer_apply(void *arg)
{
    char *text = (char *)arg;
    if (s_footer_label != NULL) {
        set_text_if_changed(s_footer_label, text);
    }
    free(text);
}

/* Safe from any task. */
void frame_ui_footer(const char *text)
{
    if (s_footer_label == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
    }
    if (on_lvgl_task()) {
        set_text_if_changed(s_footer_label, text);
        return;
    }
    char *copy = malloc(strlen(text) + 1u);
    if (copy == NULL) {
        return;
    }
    strcpy(copy, text);
    if (frame_ui_dispatch(footer_apply, copy) != ESP_OK) {
        free(copy);
    }
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

lv_obj_t *frame_ui_screen(void)
{
    return s_screen;
}

lv_obj_t *frame_ui_content(void)
{
    return s_content;
}

const frame_ui_theme_t *frame_ui_theme(void)
{
    return &s_theme;
}

void frame_ui_metrics(frame_ui_metrics_t *out)
{
    if (out != NULL) {
        *out = s_metrics;
    }
}

lv_obj_t *frame_ui_page(const char *title)
{
    if (s_content == NULL) {
        return NULL;
    }
    lv_obj_clean(s_content);
    frame_ui_set_title(title);
    frame_ui_footer("");
    frame_ui_status_progress(-1);
    return s_content;
}

void frame_ui_set_home(frame_ui_fn_t show, void *arg)
{
    s_home_fn  = show;
    s_home_arg = arg;
}

static void home_runner(void *arg)
{
    (void)arg;
    if (s_home_fn != NULL) {
        s_home_fn(s_home_arg);
    }
}

void frame_ui_home(void)
{
    /* Hops to the LVGL task unless we are already on it: home is requested
     * from the store, the shell and the WASM task when an app finishes. */
    if (on_lvgl_task()) {
        home_runner(NULL);
    } else {
        (void)frame_ui_dispatch(home_runner, NULL);
    }
}

void frame_ui_back_to_home(lv_obj_t *btn)
{
    (void)btn;
    frame_ui_home();
}

/* ------------------------------------------------------------------ */
/* Toast                                                               */
/* ------------------------------------------------------------------ */

static void toast_hide(lv_timer_t *t)
{
    (void)t;
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast       = NULL;
        s_toast_label = NULL;
    }
    s_toast_timer = NULL;
}

static void toast_apply(void *arg)
{
    char *text = (char *)arg;
    if (s_screen == NULL) {
        free(text);
        return;
    }
    if (s_toast == NULL) {
        s_toast = lv_obj_create(s_screen);
        lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_toast, c(s_theme.card_hi), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_toast, 10, 0);
        lv_obj_set_style_pad_all(s_toast, 12, 0);
        lv_obj_set_style_border_width(s_toast, 0, 0);
        lv_obj_set_style_border_color(s_toast, c(s_theme.accent), 0);
        lv_obj_set_style_border_side(s_toast, LV_BORDER_SIDE_NONE, 0);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
        s_toast_label = lv_label_create(s_toast);
        lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_toast_label, c(s_theme.text), 0);
        lv_label_set_max_width(s_toast_label, (int32_t)(s_metrics.width - 160u));
        lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(s_toast_label, text);
    free(text);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(int32_t)(FRAME_UI_FOOTER_H + 16u));
    lv_obj_move_foreground(s_toast);

    if (s_toast_timer != NULL) {
        lv_timer_reset(s_toast_timer);
    } else {
        s_toast_timer = lv_timer_create(toast_hide, 2600, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
}

void frame_ui_toast(const char *fmt, ...)
{
    char *buf = malloc(TOAST_MAX);
    if (buf == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, TOAST_MAX, fmt, ap);
    va_end(ap);

    if (on_lvgl_task()) {
        toast_apply(buf);
    } else if (frame_ui_dispatch(toast_apply, buf) != ESP_OK) {
        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Progress card                                                       */
/* ------------------------------------------------------------------ */

static void progress_build(void)
{
    s_prog_overlay = lv_obj_create(s_screen);
    lv_obj_set_size(s_prog_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_prog_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_prog_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_prog_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_prog_overlay, 0, 0);
    lv_obj_set_style_radius(s_prog_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_prog_overlay, 0, 0);
    lv_obj_clear_flag(s_prog_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_prog_overlay);
    lv_obj_set_size(card, 400, 132);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, c(s_theme.card), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_prog_title = label_at(card, "", &lv_font_montserrat_20, s_theme.text,
                            LV_ALIGN_TOP_LEFT, 0, 0);
    s_prog_bar = lv_bar_create(card);
    lv_obj_set_size(s_prog_bar, 364, 10);
    lv_obj_align(s_prog_bar, LV_ALIGN_TOP_MID, 0, 50);
    lv_bar_set_range(s_prog_bar, 0, 100);
    lv_bar_set_value(s_prog_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_prog_bar, c(s_theme.card_hi), 0);
    lv_obj_set_style_bg_opa(s_prog_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_prog_bar, c(s_theme.accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_prog_bar, LV_OPA_COVER, LV_PART_INDICATOR);

    s_prog_detail = label_at(card, "", &lv_font_montserrat_14, s_theme.text_dim,
                             LV_ALIGN_TOP_LEFT, 0, 80);
}

static void progress_apply(void *arg)
{
    typedef struct {
        int   percent;
        char  title[48];
        char  detail[96];
    } prog_msg_t;

    prog_msg_t *m = (prog_msg_t *)arg;
    if (m->percent < 0) {
        if (s_prog_overlay != NULL) {
            lv_obj_delete(s_prog_overlay);
            s_prog_overlay = NULL;
            s_prog_title   = NULL;
            s_prog_bar     = NULL;
            s_prog_detail  = NULL;
        }
        free(m);
        return;
    }
    if (s_prog_overlay == NULL) {
        progress_build();
    }
    set_text_if_changed(s_prog_title, m->title);
    set_text_if_changed(s_prog_detail, m->detail);
    lv_bar_set_value(s_prog_bar, (int32_t)m->percent, LV_ANIM_OFF);
    free(m);
}

void frame_ui_progress(const char *title, int percent, const char *detail)
{
    typedef struct {
        int   percent;
        char  title[48];
        char  detail[96];
    } prog_msg_t;

    prog_msg_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return;
    }
    m->percent = percent;
    snprintf(m->title, sizeof(m->title), "%s", title ? title : "");
    snprintf(m->detail, sizeof(m->detail), "%s", detail ? detail : "");

    if (on_lvgl_task()) {
        progress_apply(m);
    } else if (frame_ui_dispatch(progress_apply, m) != ESP_OK) {
        free(m);
    }
}

void frame_ui_progress_close(void)
{
    frame_ui_progress("", -1, "");
}

/* ------------------------------------------------------------------ */
/* Confirm dialog                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    frame_ui_confirm_cb_t cb;
    void                 *user;
    lv_obj_t              *overlay;
} confirm_t;

static void confirm_finish(confirm_t *cf, bool accepted)
{
    lv_obj_t *overlay = cf->overlay;
    frame_ui_confirm_cb_t cb  = cf->cb;
    void                 *usr = cf->user;
    free(cf);
    if (overlay != NULL) {
        lv_obj_delete(overlay);
    }
    if (cb != NULL) {
        cb(accepted, usr);
    }
}

static void confirm_accept(lv_event_t *e)
{
    confirm_finish((confirm_t *)lv_event_get_user_data(e), true);
}

static void confirm_cancel(lv_event_t *e)
{
    confirm_finish((confirm_t *)lv_event_get_user_data(e), false);
}

void frame_ui_confirm(const char *title, const char *body,
                      const char *accept_label, const char *cancel_label,
                      frame_ui_confirm_cb_t cb, void *user)
{
    if (s_screen == NULL || cb == NULL) {
        return;
    }
    confirm_t *cf = calloc(1, sizeof(*cf));
    if (cf == NULL) {
        cb(false, user);
        return;
    }
    cf->cb   = cb;
    cf->user = user;

    lv_obj_t *ov = lv_obj_create(s_screen);
    cf->overlay = ov;
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(ov);
    lv_obj_set_size(card, 440, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, c(s_theme.card), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    label_at(card, title ? title : "", &lv_font_montserrat_20, s_theme.text,
             LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bl = lv_label_create(card);
    lv_label_set_text(bl, body ? body : "");
    lv_obj_set_width(bl, 400);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, c(s_theme.text_dim), 0);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (cancel_label != NULL) {
        lv_obj_t *bc = lv_button_create(row);
        lv_obj_set_size(bc, 120, 44);
        lv_obj_set_style_bg_color(bc, c(s_theme.card_hi), 0);
        lv_obj_set_style_radius(bc, 8, 0);
        lv_obj_add_event_cb(bc, confirm_cancel, LV_EVENT_CLICKED, cf);
        label_at(bc, cancel_label, &lv_font_montserrat_20, s_theme.text, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t *ba = lv_button_create(row);
    lv_obj_set_size(ba, 140, 44);
    lv_obj_set_style_bg_color(ba, c(s_theme.danger), 0);
    lv_obj_set_style_radius(ba, 8, 0);
    lv_obj_add_event_cb(ba, confirm_accept, LV_EVENT_CLICKED, cf);
    label_at(ba, accept_label ? accept_label : "OK", &lv_font_montserrat_20, 0x0B0F14,
             LV_ALIGN_CENTER, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Shared widgets                                                      */
/* ------------------------------------------------------------------ */

lv_obj_t *frame_ui_card(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_color(o, c(s_theme.card), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *frame_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                         uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c(color), 0);
    return l;
}

lv_obj_t *frame_ui_button(lv_obj_t *parent, const char *text, uint32_t bg,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, c(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, c(s_theme.text), 0);
    lv_obj_center(l);
    if (cb != NULL) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

lv_obj_t *frame_ui_row(lv_obj_t *parent, uint16_t height)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* A flexible empty column that pushes its siblings apart in a row. */
void frame_ui_spacer(lv_obj_t *parent)
{
    lv_obj_t *sp = lv_obj_create(parent);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_set_style_pad_all(sp, 0, 0);
    lv_obj_set_width(sp, 8);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/* Small uppercase-ish section caption inside a settings list. */
lv_obj_t *frame_ui_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = frame_ui_label(parent, title, &lv_font_montserrat_14, s_theme.accent);
    lv_obj_set_style_pad_top(l, 10, 0);
    lv_obj_set_style_pad_bottom(l, 4, 0);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

/* A scrollable column filling the page, for lists and settings. */
lv_obj_t *frame_ui_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 12, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    return list;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t frame_ui_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    frame_display_info_t di;
    esp_err_t err = frame_display_get_info(&di);
    if (err != ESP_OK) {
        return err;
    }
    s_metrics.width     = di.width;
    s_metrics.height    = di.height;
    s_metrics.content_h = (uint16_t)(di.height - FRAME_UI_TOPBAR_H - FRAME_UI_FOOTER_H);

    s_dispatch = xQueueCreate(DISPATCH_LEN, sizeof(dispatch_t));
    if (s_dispatch == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, c(s_theme.bg), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);

    /* --- top bar ------------------------------------------------------ */
    s_topbar = lv_obj_create(s_screen);
    lv_obj_set_size(s_topbar, s_metrics.width, FRAME_UI_TOPBAR_H);
    lv_obj_set_pos(s_topbar, 0, 0);
    style_panel(s_topbar, s_theme.surface);

    s_back_btn = lv_button_create(s_topbar);
    lv_obj_set_size(s_back_btn, 40, 36);
    lv_obj_set_pos(s_back_btn, 8, 4);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_add_event_cb(s_back_btn, back_clicked, LV_EVENT_CLICKED, NULL);
    label_at(s_back_btn, LV_SYMBOL_LEFT, &lv_font_montserrat_28, s_theme.text,
             LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);

    s_title = label_at(s_topbar, "Frame", &lv_font_montserrat_20, s_theme.text,
                       LV_ALIGN_LEFT_MID, 16, 0);

    s_clock = label_at(s_topbar, "", &lv_font_montserrat_14, s_theme.text_dim,
                       LV_ALIGN_RIGHT_MID, -16, 0);

    s_upd_pill = lv_button_create(s_topbar);
    lv_obj_set_size(s_upd_pill, 64, 28);
    lv_obj_align(s_upd_pill, LV_ALIGN_RIGHT_MID, -92, 0);
    lv_obj_set_style_bg_color(s_upd_pill, c(s_theme.accent), 0);
    lv_obj_set_style_radius(s_upd_pill, 14, 0);
    lv_obj_set_style_pad_all(s_upd_pill, 0, 0);
    lv_obj_set_style_shadow_width(s_upd_pill, 0, 0);
    s_upd_label = lv_label_create(s_upd_pill);
    lv_obj_set_style_text_font(s_upd_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_upd_label, c(0x0B0F14), 0);
    lv_obj_center(s_upd_label);
    lv_obj_add_flag(s_upd_pill, LV_OBJ_FLAG_HIDDEN);

    s_wifi = label_at(s_topbar, LV_SYMBOL_WIFI, &lv_font_montserrat_20, s_theme.text_dim,
                      LV_ALIGN_RIGHT_MID, -168, 0);
    lv_obj_set_style_text_opa(s_wifi, LV_OPA_40, 0);

    s_prog_mini = lv_bar_create(s_topbar);
    lv_obj_set_size(s_prog_mini, 60, 6);
    lv_obj_align(s_prog_mini, LV_ALIGN_RIGHT_MID, -196, 0);
    lv_bar_set_range(s_prog_mini, 0, 100);
    lv_obj_set_style_bg_color(s_prog_mini, c(s_theme.card_hi), 0);
    lv_obj_set_style_bg_color(s_prog_mini, c(s_theme.accent), LV_PART_INDICATOR);
    lv_obj_add_flag(s_prog_mini, LV_OBJ_FLAG_HIDDEN);

    /* --- content ------------------------------------------------------ */
    s_content = lv_obj_create(s_screen);
    lv_obj_set_size(s_content, s_metrics.width, s_metrics.content_h);
    lv_obj_set_pos(s_content, 0, FRAME_UI_TOPBAR_H);
    style_panel(s_content, s_theme.bg);

    /* --- footer ------------------------------------------------------- */
    s_footer = lv_obj_create(s_screen);
    lv_obj_set_size(s_footer, s_metrics.width, FRAME_UI_FOOTER_H);
    lv_obj_set_pos(s_footer, 0, (int32_t)(s_metrics.height - FRAME_UI_FOOTER_H));
    style_panel(s_footer, s_theme.surface);
    s_footer_label = label_at(s_footer, "", &lv_font_montserrat_14, s_theme.text_dim,
                              LV_ALIGN_CENTER, 0, 0);

    lv_screen_load(s_screen);

    /* --- timers (run on the LVGL task) ------------------------------- */
    s_dispatch_timer = lv_timer_create(dispatch_drain, 15, NULL);
    if (s_dispatch_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_clock_timer = lv_timer_create(clock_tick, 10000, NULL);
    clock_tick(NULL);

    s_ready = true;
    ESP_LOGI(TAG, "ui: %ux%u, content %u px", (unsigned)s_metrics.width,
             (unsigned)s_metrics.height, (unsigned)s_metrics.content_h);
    return ESP_OK;
}
