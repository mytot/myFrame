/*
 * store_app.c — the App Store built-in (stub).
 *
 * The original store UI is not yet committed; this stub provides the two
 * symbols the launcher and app manager expect so the shell can link.
 * It shows a placeholder page; the real store will list catalog entries,
 * install/update/remove, and show progress.
 */
#include "frame_apps_builtin/apps.h"
#include "frame_ui/ui.h"
#include "frame_store/store.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "store_app";

static void back_home(void *arg)
{
    (void)arg;
    frame_ui_home();
}

void frame_app_store_show(void)
{
    lv_obj_t *page = frame_ui_page("App Store");
    if (page == NULL) {
        return;
    }
    frame_ui_set_back(back_home, NULL);
    lv_obj_t *label = lv_label_create(page);
    lv_label_set_text(label, "App Store — catalog not yet implemented.\n\nUse Settings → App store → Server to point at\nyour dev store (http://<laptop>:8080).");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    ESP_LOGI(TAG, "store placeholder shown");
}

esp_err_t frame_app_store_init(void)
{
    // Register with store events if needed; stub does nothing.
    return ESP_OK;
}
