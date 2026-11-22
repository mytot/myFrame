/*
 * Counter — the smallest useful native app.
 *
 * It shows the three things every native app needs: a page, an event handler
 * for the button, and the exit path back to the shell. Everything else (boot,
 * display, input, power, rollback confirmation) is done by frame_app.
 */
#include "frame_app/app.h"

#include "frame_ui/ui.h"

#include <stdio.h>

static lv_obj_t *s_count_label;
static lv_obj_t *s_status;
static int s_count;

static void back_to_shell(void *arg)
{
    (void)arg;
    frame_app_exit();
}

static void plus_clicked(lv_event_t *e)
{
    (void)e;
    s_count++;
    lv_label_set_text_fmt(s_count_label, "%d", s_count);
}

static void minus_clicked(lv_event_t *e)
{
    (void)e;
    if (s_count > 0) {
        s_count--;
    }
    lv_label_set_text_fmt(s_count_label, "%d", s_count);
}

static void on_event(frame_ctx_t *ctx, const frame_event_t *evt)
{
    if (evt->type == FRAME_EVT_NETWORK_UP) {
        lv_label_set_text(s_status, "network up");
    } else if (evt->type == FRAME_EVT_NETWORK_DOWN) {
        lv_label_set_text(s_status, "network down");
    }
    (void)ctx;
}

static esp_err_t on_start(frame_ctx_t *ctx)
{
    (void)ctx;
    const frame_ui_theme_t *th = frame_ui_theme();

    lv_obj_t *page = frame_ui_page("Counter");
    frame_ui_set_back(back_to_shell, NULL);
    frame_ui_footer("Native app — Back returns to the launcher");

    lv_obj_t *row = frame_ui_row(page, 140);
    s_count_label = frame_ui_label(row, "0", &lv_font_montserrat_28, th->text);
    lv_obj_set_flex_grow(s_count_label, 1);
    lv_obj_set_style_text_align(s_count_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *buttons = frame_ui_row(page, 72);
    frame_ui_spacer(buttons);
    lv_obj_t *minus = frame_ui_button(buttons, "-" LV_SYMBOL_MINUS, th->card_hi, minus_clicked, NULL);
    lv_obj_set_size(minus, 120, 56);
    lv_obj_set_style_pad_column(buttons, 16, 0);
    lv_obj_t *plus = frame_ui_button(buttons, "+" LV_SYMBOL_PLUS, th->accent, plus_clicked, NULL);
    lv_obj_set_size(plus, 120, 56);
    frame_ui_spacer(buttons);

    s_status = frame_ui_label(page, "ready", &lv_font_montserrat_14, th->text_dim);
    lv_obj_set_style_pad_left(s_status, 16, 0);

    FRAME_LOGI("counter", "started");
    return ESP_OK;
}

FRAME_APP_DEFINE("example.counter", "Counter", "1.0.0",
                 (const char *const[]){ "ui", NULL },
                 .on_start = on_start,
                 .on_event = on_event);
