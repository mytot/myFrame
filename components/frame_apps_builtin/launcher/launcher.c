#include "frame_apps_builtin/launcher.h"

#include "frame_core/app_manager.h"
#include "frame_core/event.h"
#include "frame_ui/ui.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "launcher";

#define TILE_W      160
#define TILE_H      140
#define TILE_GAP    16
#define PAGE_PAD    24
#define ICON_BOX    64
#define MAX_PAGES   8

static lv_obj_t *s_scroll;
static lv_obj_t *s_dots_row;
static lv_obj_t *s_dots[MAX_PAGES];
static size_t    s_page_count;
static bool      s_visible;
static bool      s_dirty;
static uint16_t  s_content_w;

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ------------------------------------------------------------------ */
/* Tiles                                                               */
/* ------------------------------------------------------------------ */

static bool is_store(const frame_app_state_t *app)
{
    return app->builtin && strstr(app->id, "store") != NULL;
}

static const char *builtin_symbol(const frame_app_state_t *app)
{
    if (strstr(app->id, "store") != NULL)    return LV_SYMBOL_DOWNLOAD;
    if (strstr(app->id, "settings") != NULL) return LV_SYMBOL_SETTINGS;
    if (strstr(app->id, "logs") != NULL)     return LV_SYMBOL_LIST;
    return LV_SYMBOL_DIRECTORY;
}

/* First two letters of the name, upper-cased: a cheap, crisp icon that needs
 * no image decoder and no asset storage. */
static void monogram(const char *name, char out[3])
{
    out[0] = '\0';
    out[1] = '\0';
    out[2] = '\0';
    size_t n = 0;
    for (const char *p = name; *p != '\0' && n < 2u; p++) {
        if (*p == ' ') {
            continue;
        }
        out[n++] = (char)((*p >= 'a' && *p <= 'z') ? (*p - 'a' + 'A') : *p);
    }
}

/*
 * Tiles carry the app *id*, not an index: the app manager may be re-sorted by
 * the store task between the moment the page is built and the moment the tile
 * is tapped, which would silently retarget an index.
 */
static void tile_clicked(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    for (size_t i = 0; i < frame_app_manager_count(); i++) {
        const frame_app_state_t *a = frame_app_manager_at(i);
        if (a != NULL && strcmp(a->id, id) == 0) {
            /* The shell decides what "launch" means per app kind (builtin UI,
             * native reboot-switch, WASM hot-load). The launcher only names
             * the intent. */
            (void)frame_event_post_simple(FRAME_EVT_APP_LAUNCH, (int32_t)i, 0);
            return;
        }
    }
}

static void tile_deleted(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void tile_create(lv_obj_t *parent, const frame_app_state_t *app)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    const bool store = is_store(app);

    lv_obj_t *t = lv_button_create(parent);
    lv_obj_set_size(t, TILE_W, TILE_H);
    lv_obj_set_style_bg_color(t, c(store ? th->accent : th->card), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_set_style_shadow_width(t, 0, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_border_opa(t, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(t, c(store ? th->accent : th->card_hi), LV_STATE_PRESSED);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 8, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    char *id_copy = malloc(strlen(app->id) + 1u);
    if (id_copy == NULL) {
        lv_obj_delete(t);
        return;
    }
    strcpy(id_copy, app->id);
    lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, id_copy);
    lv_obj_add_event_cb(t, tile_deleted, LV_EVENT_DELETE, id_copy);

    /* Icon box. */
    lv_obj_t *box = lv_obj_create(t);
    lv_obj_set_size(box, ICON_BOX, ICON_BOX);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_bg_color(box, c(store ? 0x0B0F14 : th->accent2), 0);
    lv_obj_set_style_bg_opa(box, store ? LV_OPA_20 : LV_OPA_20, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(box);
    lv_obj_set_style_text_color(icon, c(store ? 0x0B0F14 : th->accent2), 0);
    if (app->builtin) {
        lv_label_set_text(icon, builtin_symbol(app));
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    } else {
        char mg[3];
        monogram(app->name, mg);
        lv_label_set_text(icon, mg);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_letter_space(icon, 1, 0);
    }
    lv_obj_center(icon);

    /* Name. */
    lv_obj_t *name = lv_label_create(t);
    lv_label_set_text(name, app->name);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, c(store ? 0x0B0F14 : th->text), 0);
    lv_label_set_max_width(name, TILE_W - 24);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

    /* Update badge. */
    if (app->update_available) {
        lv_obj_t *badge = lv_obj_create(t);
        lv_obj_set_size(badge, 22, 22);
        lv_obj_set_pos(badge, TILE_W - 32, -2);
        lv_obj_set_style_radius(badge, 11, 0);
        lv_obj_set_style_bg_color(badge, c(store ? 0x0B0F14 : th->accent), 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *bl = lv_label_create(badge);
        lv_label_set_text(bl, "!");
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(bl, c(store ? th->accent : 0x0B0F14), 0);
        lv_obj_center(bl);
    }

    /* Version + kind, dim, so the launcher doubles as a quick inventory. */
    char sub[48];
    snprintf(sub, sizeof(sub), "%s%s", app->version,
             app->kind == FRAME_APP_KIND_MANAGED ? "  " LV_SYMBOL_SHUFFLE
             : app->kind == FRAME_APP_KIND_NATIVE ? "  " LV_SYMBOL_DRIVE : "");
    lv_obj_t *ver = lv_label_create(t);
    lv_label_set_text(ver, sub);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ver, c(store ? 0x0B0F14 : th->text_dim), 0);
    lv_obj_set_style_text_opa(ver, store ? LV_OPA_70 : LV_OPA_COVER, 0);

    if (!app->healthy) {
        lv_obj_t *warn = lv_label_create(t);
        lv_label_set_text(warn, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(warn, c(store ? 0x0B0F14 : th->danger), 0);
        lv_obj_set_pos(warn, 6, -2);
    }
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static void dots_update(size_t active)
{
    const frame_ui_theme_t *th = frame_ui_theme();
    for (size_t i = 0; i < s_page_count && i < MAX_PAGES; i++) {
        if (s_dots[i] == NULL) {
            continue;
        }
        lv_obj_set_style_bg_color(s_dots[i], c(i == active ? th->accent : th->text_dim), 0);
        lv_obj_set_style_bg_opa(s_dots[i], i == active ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void scroll_cb(lv_event_t *e)
{
    lv_obj_t *sc = lv_event_get_target(e);
    const int32_t x = lv_obj_get_scroll_x(sc);
    size_t active = 0;
    if (s_content_w > 0u) {
        active = (size_t)((-x + (int32_t)(s_content_w / 2u)) / (int32_t)s_content_w);
    }
    if (active >= s_page_count) {
        active = s_page_count ? s_page_count - 1u : 0u;
    }
    dots_update(active);
}

static void dots_build(lv_obj_t *parent, size_t pages)
{
    if (s_dots_row != NULL) {
        lv_obj_delete(s_dots_row);
    }
    s_page_count = pages < MAX_PAGES ? pages : MAX_PAGES;
    memset(s_dots, 0, sizeof(s_dots));

    if (pages <= 1u) {
        s_dots_row = NULL;
        return;
    }

    const frame_ui_theme_t *th = frame_ui_theme();
    s_dots_row = lv_obj_create(parent);
    lv_obj_set_size(s_dots_row, LV_SIZE_CONTENT, 10);
    lv_obj_align(s_dots_row, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(s_dots_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots_row, 0, 0);
    lv_obj_set_style_pad_all(s_dots_row, 0, 0);
    lv_obj_set_style_pad_column(s_dots_row, 8, 0);
    lv_obj_set_flex_flow(s_dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(s_dots_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < s_page_count; i++) {
        s_dots[i] = lv_obj_create(s_dots_row);
        lv_obj_set_size(s_dots[i], 8, 8);
        lv_obj_set_style_radius(s_dots[i], 4, 0);
        lv_obj_set_style_bg_color(s_dots[i], c(th->text_dim), 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_40, 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_set_style_pad_all(s_dots[i], 0, 0);
        lv_obj_clear_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    dots_update(0);
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static void build(void *arg)
{
    (void)arg;
    const frame_ui_theme_t *th = frame_ui_theme();
    frame_ui_metrics_t m;
    frame_ui_metrics(&m);
    s_content_w = m.width;

    lv_obj_t *content = frame_ui_page("Frame");
    if (content == NULL) {
        return;
    }
    frame_ui_clear_back();
    frame_ui_footer("Swipe for more  " LV_SYMBOL_RIGHT);

    /* Tiles per page from the real content height, so the layout holds on a
     * different-sized board without touching this file. */
    const int32_t usable_h = (int32_t)m.content_h - 2 * PAGE_PAD - 12;  /* 12: dots */
    const int32_t rows = (usable_h + TILE_GAP) / (TILE_H + TILE_GAP);
    const int32_t cols = ((int32_t)m.width - 2 * PAGE_PAD + TILE_GAP) / (TILE_W + TILE_GAP);
    const size_t per_page = (size_t)((rows > 0 ? rows : 1) * (cols > 0 ? cols : 1));

    const size_t count = frame_app_manager_count();
    if (count == 0u) {
        lv_obj_t *l = lv_label_create(content);
        lv_label_set_text(l, "No apps installed");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, c(th->text_dim), 0);
        lv_obj_center(l);
        return;
    }

    const size_t pages = (count + per_page - 1u) / per_page;

    s_scroll = lv_obj_create(content);
    lv_obj_set_size(s_scroll, m.width, m.content_h);
    lv_obj_set_pos(s_scroll, 0, 0);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_scroll, LV_SCROLL_SNAP_START);
    lv_obj_add_event_cb(s_scroll, scroll_cb, LV_EVENT_SCROLL, NULL);

    size_t index = 0;
    for (size_t p = 0; p < pages; p++) {
        lv_obj_t *pg = lv_obj_create(s_scroll);
        lv_obj_set_size(pg, m.width, m.content_h);
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, PAGE_PAD);
        lv_obj_set_style_pad_row(pg, TILE_GAP);
        lv_obj_set_style_pad_column(pg, TILE_GAP);
        lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(pg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        for (size_t k = 0; k < per_page && index < count; k++, index++) {
            const frame_app_state_t *app = frame_app_manager_at(index);
            if (app != NULL) {
                tile_create(pg, app);
            }
        }
    }

    dots_build(content, pages);
    ESP_LOGI(TAG, "home: %u apps on %u page(s), %u per page",
             (unsigned)count, (unsigned)pages, (unsigned)per_page);
}

/* ------------------------------------------------------------------ */

void frame_launcher_show(void)
{
    s_visible = true;
    build(NULL);
    s_dirty = false;
}

static void refresh_runner(void *arg)
{
    (void)arg;
    if (s_visible) {
        build(NULL);
    }
    s_dirty = false;
}

void frame_launcher_refresh(void)
{
    /* Always through the dispatch queue: the LVGL task is the only task that
     * may build widgets, and it is not the only task on core 1. */
    s_dirty = true;
    (void)frame_ui_dispatch(refresh_runner, NULL);
}

void frame_launcher_hide(void)
{
    s_visible = false;
}

bool frame_launcher_visible(void)
{
    return s_visible;
}

static void on_event(const frame_event_t *evt, void *user)
{
    (void)user;
    if (evt->type == FRAME_EVT_APP_INSTALLED || evt->type == FRAME_EVT_APP_REMOVED ||
        evt->type == FRAME_EVT_STORE_UPDATED || evt->type == FRAME_EVT_SETTINGS_CHANGED) {
        frame_launcher_refresh();
    }
}

esp_err_t frame_launcher_init(void)
{
    frame_ui_set_home(frame_launcher_show, NULL);
    esp_err_t err = frame_event_subscribe(FRAME_EVT_NONE, on_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event subscription failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
