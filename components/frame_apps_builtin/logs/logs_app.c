/*
 * logs_app.c — the Logs built-in: a live view of the PSRAM log ring.
 *
 * Reading is cheap (frame_log_query copies out of the ring under the logger's
 * lock) so the page refreshes once a second while it is open, newest first,
 * with a level filter. Nothing is written to flash; the ring is volatile by
 * design.
 */
#include "frame_apps_builtin/apps.h"

#include "frame_core/event.h"
#include "frame_log/log.h"
#include "frame_ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "logs_app";

#define LOG_LINES_MAX 120u

static lv_obj_t *s_list;
static lv_obj_t *s_filter_buttons[4];
static frame_log_level_t s_level = FRAME_LOG_DEBUG;
static lv_timer_t *s_timer;
static bool s_visible;

static const struct {
    const char          *label;
    frame_log_level_t    level;
} k_filters[] = {
    { "All",     FRAME_LOG_DEBUG },
    { "Info+",   FRAME_LOG_INFO  },
    { "Warn+",   FRAME_LOG_WARN  },
    { "Errors",  FRAME_LOG_ERROR },
};

static const char *level_name(frame_log_level_t l)
{
    switch (l) {
    case FRAME_LOG_ERROR: return "E";
    case FRAME_LOG_WARN:  return "W";
    case FRAME_LOG_INFO:  return "I";
    default:              return "D";
    }
}

static lv_color_t level_color(frame_log_level_t l)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    switch (l) {
    case FRAME_LOG_ERROR: return lv_color_hex(th->danger);
    case FRAME_LOG_WARN:  return lv_color_hex(0xfbbf24);
    case FRAME_LOG_INFO:  return lv_color_hex(th->accent2);
    default:              return lv_color_hex(th->text_dim);
    }
}

static void rebuild(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list)) {
        return;
    }
    lv_obj_clean(s_list);

    frame_log_line_t *lines = heap_caps_malloc(
        sizeof(frame_log_line_t) * LOG_LINES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lines == NULL) {
        return;
    }
    const size_t n = frame_log_query(s_level, NULL, lines, LOG_LINES_MAX);

    for (size_t i = 0; i < n; i++) {
        const frame_log_line_t *l = &lines[i];
        const uint32_t secs = (uint32_t)(l->timestamp_us / 1000000ull);
        char buf[FRAME_LOG_LINE_MAX + 48];
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u  %s %-*s %s",
                 (unsigned)((secs / 3600u) % 100u), (unsigned)((secs / 60u) % 60u),
                 (unsigned)(secs % 60u), level_name(l->level), FRAME_LOG_TAG_MAX - 1u,
                 l->tag, l->text);

        lv_obj_t *row = lv_label_create(s_list);
        lv_label_set_text(row, buf);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(row, level_color(l->level), 0);
        lv_obj_set_width(row, LV_PCT(100));
        lv_label_set_long_mode(row, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_pad_ver(row, 2, 0);
    }
    free(lines);

    frame_log_stats_t st;
    frame_log_get_stats(&st);
    char foot[96];
    snprintf(foot, sizeof(foot), "Newest first " LV_SYMBOL_REFRESH " 1 s   ring %u KB   %u dropped",
             (unsigned)(st.ring_bytes / 1024u), (unsigned)st.dropped);
    frame_ui_footer(foot);
}

static void back_home(void *arg)
{
    (void)arg;
    frame_ui_home();
}

static void refresh_cb(lv_timer_t *t)
{
    if (!s_visible || s_list == NULL || !lv_obj_is_valid(s_list)) {
        lv_timer_delete(t);
        s_timer = NULL;
        return;
    }
    rebuild();
}

static void filter_clicked(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    s_level = k_filters[idx].level;
    const frame_ui_theme_t *th = frame_ui_theme();
    for (size_t i = 0; i < 4u; i++) {
        lv_obj_set_style_bg_color(s_filter_buttons[i],
                                  lv_color_hex(i == idx ? th->accent : th->card_hi), 0);
    }
    rebuild();
}

static void clear_clicked(lv_event_t *e)
{
    (void)e;
    frame_log_clear();
    rebuild();
}

void frame_app_logs_show(void)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    frame_ui_metrics_t m;
    frame_ui_metrics(&m);

    lv_obj_t *page = frame_ui_page("Logs");
    if (page == NULL) {
        return;
    }
    frame_ui_set_back(back_home, NULL);
    s_visible = true;

    lv_obj_t *bar = frame_ui_row(page, 44);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 12, 0);
    for (size_t i = 0; i < 4u; i++) {
        s_filter_buttons[i] = frame_ui_button(bar, k_filters[i].label, th->card_hi,
                                              filter_clicked, (void *)(uintptr_t)i);
        lv_obj_set_size(s_filter_buttons[i], 92, 36);
        lv_obj_set_style_bg_color(s_filter_buttons[i],
            lv_color_hex(k_filters[i].level == s_level ? th->accent : th->card_hi), 0);
    }
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *clear = frame_ui_button(bar, LV_SYMBOL_TRASH " Clear", th->card_hi, clear_clicked, NULL);
    lv_obj_set_size(clear, 110, 36);

    s_list = lv_obj_create(page);
    lv_obj_set_size(s_list, LV_PCT(100), (int32_t)m.content_h - 44 - 8);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    rebuild();
    if (s_timer == NULL) {
        s_timer = lv_timer_create(refresh_cb, 1000, NULL);
    } else {
        lv_timer_resume(s_timer);
    }
}
