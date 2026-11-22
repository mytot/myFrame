/*
 * settings_app.c — the Settings built-in.
 *
 * One scrollable column of rows grouped into sections. Every control writes
 * through the owning component's API (frame_power_*, frame_store_*,
 * frame_settings_*) rather than poking NVS directly, so the policy stays in
 * one place. Text entry uses LVGL's on-screen keyboard: the device has no
 * other way to type a Wi-Fi password.
 *
 * Everything here runs on the LVGL task. Network actions (connect, scan) are
 * non-blocking calls into frame_net and report back through events.
 */
#include "frame_apps_builtin/apps.h"

#include "frame_core/app.h"
#include "frame_core/board_types.h"
#include "frame_core/event.h"
#include "frame_core/power.h"
#include "frame_log/log.h"
#include "frame_net/net.h"
#include "frame_storage/storage.h"
#include "frame_store/store.h"
#include "frame_ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "settings";

#define KEY_WIFI_SSID  "wifi_ssid"
#define KEY_WIFI_PASS  "wifi_pass"
#define KEY_POWER_OFF  "power_off_ms"
#define KEY_BRIGHTNESS "brightness"

/* Stored as 0 for "never"; the ladder itself uses a value no idle period can
 * reach, so frame_power_tick needs no special case. */
#define POWER_OFF_NEVER 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void back_home(void *arg)
{
    (void)arg;
    frame_ui_home();
}

static lv_obj_t *row_with_label(lv_obj_t *list, const char *text, uint16_t h)
{
    lv_obj_t *row = frame_ui_row(list, h);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    frame_ui_label(row, text, &lv_font_montserrat_14, frame_ui_theme()->text);
    frame_ui_spacer(row);
    return row;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, bool primary,
                              lv_event_cb_t cb, void *user)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    lv_obj_t *b = frame_ui_button(parent, text, primary ? th->accent : th->card_hi, cb, user);
    lv_obj_set_height(b, 36);
    lv_obj_set_style_pad_left(b, 14, 0);
    lv_obj_set_style_pad_right(b, 14, 0);
    return b;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi status                                                        */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_wifi_status;

static void wifi_status_text(char *out, size_t cap)
{
    frame_net_status_t st;
    frame_net_get_status(&st);
    switch (st.state) {
    case FRAME_NET_CONNECTED:
        snprintf(out, cap, "Connected to %s  " LV_SYMBOL_WIFI " %d dBm  %s",
                 st.ssid, (int)st.rssi, st.ip);
        break;
    case FRAME_NET_CONNECTING:
        snprintf(out, cap, "Connecting to %s" LV_SYMBOL_DOTS, st.ssid);
        break;
    default:
        snprintf(out, cap, "Not connected%s", st.ssid[0] ? "  (retrying paused)" : "");
        break;
    }
}

static void refresh_wifi_status(void)
{
    char buf[96];
    wifi_status_text(buf, sizeof(buf));
    if (s_wifi_status != NULL && lv_obj_is_valid(s_wifi_status)) {
        lv_label_set_text(s_wifi_status, buf);
    }
}

/* ------------------------------------------------------------------ */
/* Wi-Fi entry page (keyboard)                                         */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_kb;
static lv_obj_t  *s_ta_ssid;
static lv_obj_t  *s_ta_pass;

static void kb_def_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);
    if (code == LV_EVENT_CANCEL) {
        /* Hide the keyboard and drop focus so the page underneath is usable. */
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ta_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = lv_event_get_user_data(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        if (lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_KEYPAD) {
            lv_keyboard_set_textarea(kb, ta);
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_READY) {
        /* Enter on a one-line textarea: move to the next field, or connect. */
        if (ta == s_ta_ssid) {
            lv_obj_focus(s_ta_pass);
            lv_keyboard_set_textarea(kb, s_ta_pass);
        }
    }
}

static void wifi_connect_clicked(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(s_ta_ssid);
    const char *pass = lv_textarea_get_text(s_ta_pass);
    if (ssid == NULL || ssid[0] == '\0') {
        frame_ui_toast("Enter a network name");
        return;
    }
    esp_err_t err = frame_settings_set_str(KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = frame_settings_set_str(KEY_WIFI_PASS, pass ? pass : "");
    }
    if (err != ESP_OK) {
        frame_ui_toast("Could not save Wi-Fi settings");
        return;
    }
    err = frame_net_connect(ssid, pass);
    frame_ui_toast(err == ESP_OK ? "Connecting to %s" : "Wi-Fi error: %s",
                   err == ESP_OK ? ssid : esp_err_to_name(err));
    FRAME_LOGI(TAG, "wi-fi connect requested for %s", ssid);
}

static void wifi_disconnect_clicked(lv_event_t *e)
{
    (void)e;
    (void)frame_net_disconnect();
    (void)frame_settings_set_str(KEY_WIFI_SSID, "");
    frame_ui_toast("Wi-Fi disconnected");
}

static void wifi_back(void *arg)
{
    (void)arg;
    frame_app_settings_show();
}

/* Rebuilds the Wi-Fi page after a scan so the list is up to date. */
static void wifi_page_show(void)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    frame_ui_metrics_t m;
    frame_ui_metrics(&m);

    lv_obj_t *page = frame_ui_page("Wi-Fi");
    if (page == NULL) {
        return;
    }
    frame_ui_set_back(wifi_back, NULL);

    /* Left: fields. Right: keyboard. On a 800 px wide screen the split is
     * 300 / 500; on a narrower board the keyboard goes below. */
    const bool side_by_side = m.width >= 720u;
    lv_obj_t *form = frame_ui_list(page);
    if (side_by_side) {
        lv_obj_set_size(form, 300, LV_PCT(100));
        lv_obj_clear_flag(form, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    }
    lv_obj_set_style_pad_row(form, 10, 0);

    s_wifi_status = frame_ui_label(form, "", &lv_font_montserrat_14, th->text_dim);
    lv_obj_set_width(s_wifi_status, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_WRAP);
    refresh_wifi_status();

    frame_ui_section(form, "Network name");
    s_ta_ssid = lv_textarea_create(form);
    lv_textarea_set_one_line(s_ta_ssid, true);
    lv_textarea_set_placeholder_text(s_ta_ssid, "SSID");
    lv_textarea_set_max_length(s_ta_ssid, 32);
    lv_obj_set_width(s_ta_ssid, LV_PCT(100));
    lv_obj_set_height(s_ta_ssid, 44);
    lv_obj_set_style_text_font(s_ta_ssid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(s_ta_ssid, lv_color_hex(th->accent2), LV_STATE_FOCUSED);

    frame_ui_section(form, "Password");
    s_ta_pass = lv_textarea_create(form);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "passphrase");
    lv_textarea_set_max_length(s_ta_pass, 64);
    lv_obj_set_width(s_ta_pass, LV_PCT(100));
    lv_obj_set_height(s_ta_pass, 44);
    lv_obj_set_style_text_font(s_ta_pass, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(s_ta_pass, lv_color_hex(th->accent2), LV_STATE_FOCUSED);

    char saved[33] = { 0 };
    if (frame_settings_get_str(KEY_WIFI_SSID, saved, sizeof(saved), NULL) == ESP_OK) {
        lv_textarea_set_text(s_ta_ssid, saved);
    }

    lv_obj_t *buttons = frame_ui_row(form, 44);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    small_button(buttons, LV_SYMBOL_WIFI " Connect", true, wifi_connect_clicked, NULL);
    small_button(buttons, "Disconnect", false, wifi_disconnect_clicked, NULL);
    frame_ui_spacer(buttons);

    /* Keyboard. */
    s_kb = lv_keyboard_create(page);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_kb, kb_def_event, LV_EVENT_CANCEL, NULL);
    if (side_by_side) {
        lv_obj_set_size(s_kb, m.width - 300u, LV_PCT(100));
        lv_obj_align(s_kb, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_set_size(s_kb, LV_PCT(100), m.height * 55u / 100u);
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(form, m.content_h - m.height * 55u / 100u);
    }
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_ta_ssid, ta_event, LV_EVENT_ALL, s_kb);
    lv_obj_add_event_cb(s_ta_pass, ta_event, LV_EVENT_ALL, s_kb);
}

/* ------------------------------------------------------------------ */
/* Brightness / sleep                                                   */
/* ------------------------------------------------------------------ */

static void brightness_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const int v = (int)lv_slider_get_value(slider);
    frame_power_set_active_level((uint8_t)v);
    (void)frame_settings_set_u32(KEY_BRIGHTNESS, (uint32_t)v);
    frame_power_activity();
}

static void apply_power_policy(uint32_t off_ms)
{
    frame_power_policy_t p = FRAME_POWER_POLICY_DEFAULT;
    if (off_ms == 0u || off_ms > 24u * 3600u * 1000u) {
        off_ms = POWER_OFF_NEVER;
    }
    p.off_after_ms  = off_ms;
    p.idle_after_ms = (off_ms == POWER_OFF_NEVER) ? POWER_OFF_NEVER : off_ms / 3u;
    p.dim_after_ms  = (off_ms == POWER_OFF_NEVER) ? POWER_OFF_NEVER : off_ms / 6u;
    (void)frame_power_set_policy(&p);
    (void)frame_settings_set_u32(KEY_POWER_OFF, off_ms);
}

static void sleep_option_clicked(lv_event_t *e)
{
    apply_power_policy((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    const uint32_t ms = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    frame_ui_toast(ms ? "Screen off after %u min" : "Screen stays on",
                   (unsigned)(ms / 60000u));
}

/* ------------------------------------------------------------------ */
/* Store URL page                                                       */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_ta_url;

static void url_back(void *arg)
{
    (void)arg;
    frame_app_settings_show();
}

static void url_save_clicked(lv_event_t *e)
{
    (void)e;
    const char *url = lv_textarea_get_text(s_ta_url);
    esp_err_t err = frame_store_set_url(url);
    frame_ui_toast(err == ESP_OK ? "Store address saved" : "Invalid store address");
    if (err == ESP_OK) {
        frame_app_settings_show();
    }
}

static void store_url_page_show(void)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    frame_ui_metrics_t m;
    frame_ui_metrics(&m);

    lv_obj_t *page = frame_ui_page("App Store address");
    if (page == NULL) {
        return;
    }
    frame_ui_set_back(url_back, NULL);

    lv_obj_t *form = frame_ui_list(page);
    lv_obj_set_size(form, LV_PCT(100), m.content_h - m.height * 45u / 100u);
    frame_ui_label(form, "Where the device fetches catalog.json, catalog.sig and packages. "
                         "A plain http:// address on your LAN is fine for development.",
                   &lv_font_montserrat_14, th->text_dim);
    s_ta_url = lv_textarea_create(form);
    lv_textarea_set_one_line(s_ta_url, true);
    lv_textarea_set_max_length(s_ta_url, FRAME_STORE_URL_MAX - 1u);
    lv_obj_set_width(s_ta_url, LV_PCT(100));
    lv_obj_set_height(s_ta_url, 44);
    lv_textarea_set_text(s_ta_url, frame_store_url());

    lv_obj_t *buttons = frame_ui_row(form, 44);
    small_button(buttons, "Save", true, url_save_clicked, NULL);

    lv_obj_t *kb = lv_keyboard_create(page);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(kb, LV_PCT(100), m.height * 45u / 100u);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, kb_def_event, LV_EVENT_CANCEL, NULL);
    lv_keyboard_set_textarea(kb, s_ta_url);
    lv_obj_add_event_cb(s_ta_url, ta_event, LV_EVENT_ALL, kb);
}

/* ------------------------------------------------------------------ */
/* Root settings page                                                   */
/* ------------------------------------------------------------------ */

static void open_wifi(lv_event_t *e)
{
    (void)e;
    wifi_page_show();
}

static void open_store_url(lv_event_t *e)
{
    (void)e;
    store_url_page_show();
}

static void refresh_catalog_clicked(lv_event_t *e)
{
    (void)e;
    frame_store_request_t r = { .cmd = FRAME_STORE_CMD_REFRESH };
    esp_err_t err = frame_store_request(&r);
    frame_ui_toast(err == ESP_OK ? "Refreshing catalog" : "Store is busy");
}

static void factory_reset_clicked(lv_event_t *e)
{
    (void)e;
    /* Only settings are cleared; installed apps stay, because removing them
     * means erasing flash partitions and that deserves its own confirmation. */
    frame_settings_reset_all();
    frame_ui_toast("Settings cleared. Reboot to apply.");
}

void frame_app_settings_show(void)
{
    const frame_ui_theme_t *th = frame_ui_theme();

    lv_obj_t *page = frame_ui_page("Settings");
    if (page == NULL) {
        return;
    }
    frame_ui_set_back(back_home, NULL);
    frame_ui_footer("Changes apply immediately");

    lv_obj_t *list = frame_ui_list(page);

    /* --- Wi-Fi ------------------------------------------------------ */
    frame_ui_section(list, "Wi-Fi");
    {
        lv_obj_t *row = row_with_label(list, "Network", 56);
        s_wifi_status = frame_ui_label(row, "", &lv_font_montserrat_14, th->text_dim);
        lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_wifi_status, 300);
        refresh_wifi_status();
        small_button(row, "Change", true, open_wifi, NULL);
    }

    /* --- Display ---------------------------------------------------- */
    frame_ui_section(list, "Display");
    {
        lv_obj_t *row = row_with_label(list, "Brightness", 56);
        lv_obj_t *slider = lv_slider_create(row);
        lv_obj_set_width(slider, 260);
        lv_slider_set_range(slider, 5, 100);
        uint32_t saved = 0u;
        const uint8_t cur = frame_power_active_level();
        if (frame_settings_get_u32(KEY_BRIGHTNESS, &saved, cur) == ESP_OK) {
            lv_slider_set_value(slider, (int32_t)saved, LV_ANIM_OFF);
        } else {
            lv_slider_set_value(slider, cur, LV_ANIM_OFF);
        }
        lv_obj_set_style_bg_color(slider, lv_color_hex(th->accent), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(th->card_hi), LV_PART_MAIN);
        lv_obj_add_event_cb(slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* --- Power ------------------------------------------------------ */
    frame_ui_section(list, "Power  —  screen off after");
    {
        lv_obj_t *row = frame_ui_row(list, 44);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        static const struct { const char *label; uint32_t ms; } opts[] = {
            { "1 min",   60000u },
            { "5 min",  300000u },
            { "10 min", 600000u },
            { "Never",  0u },
        };
        uint32_t off_ms = FRAME_POWER_POLICY_DEFAULT.off_after_ms;
        (void)frame_settings_get_u32(KEY_POWER_OFF, &off_ms, off_ms);
        for (size_t i = 0; i < 4u; i++) {
            const bool on = (off_ms == opts[i].ms);
            lv_obj_t *b = small_button(row, opts[i].label, on, sleep_option_clicked,
                                       (void *)(uintptr_t)opts[i].ms);
            if (!on) {
                lv_obj_set_style_bg_color(b, lv_color_hex(th->card_hi), 0);
            }
        }
        frame_ui_spacer(row);
    }

    /* --- App store -------------------------------------------------- */
    frame_ui_section(list, "App store");
    {
        lv_obj_t *row = row_with_label(list, "Server", 56);
        lv_obj_t *url = frame_ui_label(row, frame_store_url(), &lv_font_montserrat_14, th->text_dim);
        lv_label_set_long_mode(url, LV_LABEL_LONG_DOT);
        lv_obj_set_width(url, 320);
        small_button(row, "Edit", false, open_store_url, NULL);

        lv_obj_t *row2 = row_with_label(list, "Catalog", 56);
        char cat[64];
        snprintf(cat, sizeof(cat), "%s, %u app%s  %s",
                 frame_store_catalog_valid() ? "loaded" : "not loaded",
                 (unsigned)frame_store_catalog_count(),
                 frame_store_catalog_count() == 1u ? "" : "s", frame_store_state_str(frame_store_state()));
        frame_ui_label(row2, cat, &lv_font_montserrat_14, th->text_dim);
        small_button(row2, LV_SYMBOL_REFRESH " Refresh", false, refresh_catalog_clicked, NULL);
    }

    /* --- Developer -------------------------------------------------- */
    frame_ui_section(list, "Developer");
    {
        lv_obj_t *row = row_with_label(list, "Build", 56);
        char info[96];
        const frame_board_t *board = frame_board();
        snprintf(info, sizeof(info), "Frame SDK %s  " LV_SYMBOL_DRIVE " %s  %s",
                 FRAME_SDK_VERSION, board ? board->id : "?", board ? board->revision : "");
        frame_ui_label(row, info, &lv_font_montserrat_14, th->text_dim);

        lv_obj_t *row2 = row_with_label(list, "Memory", 56);
        snprintf(info, sizeof(info), "heap %u KB free  " LV_SYMBOL_SD_CARD " %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
        frame_ui_label(row2, info, &lv_font_montserrat_14, th->text_dim);

        lv_obj_t *row3 = row_with_label(list, "Signature policy", 56);
        frame_ui_label(row3, frame_store_unsigned_allowed()
                                  ? "Unsigned packages accepted (dev build)"
                                  : "Ed25519 signatures required",
                       &lv_font_montserrat_14,
                       frame_store_unsigned_allowed() ? th->danger : th->ok);

        lv_obj_t *row4 = row_with_label(list, "Uptime", 56);
        snprintf(info, sizeof(info), "%llu s", (unsigned long long)(esp_timer_get_time() / 1000000ull));
        frame_ui_label(row4, info, &lv_font_montserrat_14, th->text_dim);
    }

    /* --- About ------------------------------------------------------ */
    frame_ui_section(list, "About");
    {
        lv_obj_t *row = row_with_label(list, "FrameOS", 56);
        char about[96];
        if (board != NULL && board->display != NULL) {
            snprintf(about, sizeof(about), "%s  " LV_SYMBOL_IMAGE " %ux%u @ %u Hz  %s",
                     board->chip == FRAME_CHIP_ESP32S3 ? "ESP32-S3" : frame_chip_str(board->chip),
                     (unsigned)board->display->width, (unsigned)board->display->height,
                     (unsigned)board->display->refresh_hz,
                     frame_power_policy()->off_after_ms != POWER_OFF_NEVER ? "idle ladder on"
                                                                              : "idle ladder off");
        } else {
            snprintf(about, sizeof(about), "FrameOS");
        }
        frame_ui_label(row, about, &lv_font_montserrat_14, th->text_dim);

        lv_obj_t *row2 = row_with_label(list, "Settings", 56);
        small_button(row2, "Clear all", false, factory_reset_clicked, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Event glue: keep the Wi-Fi line honest while the page is open        */
/* ------------------------------------------------------------------ */

static void on_event(const frame_event_t *evt, void *user)
{
    (void)user;
    if ((evt->type == FRAME_EVT_NETWORK_UP || evt->type == FRAME_EVT_NETWORK_DOWN ||
         evt->type == FRAME_EVT_SETTINGS_CHANGED) &&
        s_wifi_status != NULL && lv_obj_is_valid(s_wifi_status)) {
        refresh_wifi_status();
    }
}

esp_err_t frame_app_settings_init(void)
{
    /* Restore the brightness the user chose. */
    uint32_t bright = 0u;
    if (frame_settings_get_u32(KEY_BRIGHTNESS, &bright, 100u) == ESP_OK && bright >= 1u && bright <= 100u) {
        frame_power_set_active_level((uint8_t)bright);
    }
    uint32_t off_ms = FRAME_POWER_POLICY_DEFAULT.off_after_ms;
    (void)frame_settings_get_u32(KEY_POWER_OFF, &off_ms, off_ms);
    apply_power_policy(off_ms);

    esp_err_t err = frame_event_subscribe(FRAME_EVT_NONE, on_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event subscription failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

/* Called from app_main: bring Wi-Fi up with the saved credentials, if any. */
void frame_app_settings_apply_wifi(void)
{
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (frame_settings_get_str(KEY_WIFI_SSID, ssid, sizeof(ssid), NULL) != ESP_OK || ssid[0] == '\0') {
        return;
    }
    (void)frame_settings_get_str(KEY_WIFI_PASS, pass, sizeof(pass), NULL);
    esp_err_t err = frame_net_connect(ssid, pass);
    FRAME_LOGI(TAG, "wi-fi %s (%s)", err == ESP_OK ? "connecting" : "failed", ssid);
}
