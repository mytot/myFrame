#include "frame_shell/shell.h"

#include "frame_apps_builtin/apps.h"
#include "frame_apps_builtin/launcher.h"
#include "frame_core/app.h"
#include "frame_core/app_manager.h"
#include "frame_core/event.h"
#include "frame_display/display.h"
#include "frame_log/log.h"
#include "frame_ota/ota.h"
#include "frame_ui/ui.h"
#include "frame_wasm/wasm.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "shell";

#define LAUNCH_QUEUE_LEN 4u

typedef struct {
    char id[FRAME_APP_ID_MAX];
    bool home;
} launch_req_t;

static QueueHandle_t s_queue;

/* ------------------------------------------------------------------ */
/* Worker                                                               */
/* ------------------------------------------------------------------ */

static void show_builtin(void *arg)
{
    const char *id = (const char *)arg;
    (void)frame_apps_open(id);
}

static void go_home(void *arg)
{
    (void)arg;
    frame_launcher_show();
}

static void launch_native(const frame_app_state_t *app)
{
    ESP_LOGI(TAG, "switching boot partition to %s (slot %u) and restarting",
             app->id, (unsigned)app->slot);
    frame_app_manager_mark_launched(app->id, (uint32_t)(esp_timer_get_time() / 1000));

    const int slot = frame_ota_set_boot_app(app->id);
    if (slot < 0) {
        ESP_LOGE(TAG, "cannot set boot partition for %s", app->id);
        frame_app_manager_mark_unhealthy(app->id);
        frame_ui_toast("Cannot launch %s", app->name);
        return;
    }
    /* Persist the slot table so the app's "last launched" bookkeeping and
     * any reservation state survive the restart. */
    (void)frame_ota_save();
    frame_ui_progress("Starting", 100, app->name);
    vTaskDelay(pdMS_TO_TICKS(150));     /* let the frame reach the panel */
    esp_restart();
}

static void launch_managed(const frame_app_state_t *app)
{
    frame_app_manager_mark_launched(app->id, (uint32_t)(esp_timer_get_time() / 1000));
    frame_launcher_hide();
    frame_ui_progress("Loading", 30, app->name);

    const esp_err_t rc = frame_wasm_launch(app->id);
    frame_ui_progress_close();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "managed launch failed for %s: %s", app->id, esp_err_to_name(rc));
        frame_app_manager_mark_unhealthy(app->id);
        frame_ui_toast("%s failed to load", app->name);
        (void)frame_ui_dispatch(go_home, NULL);
    } else {
        frame_app_manager_mark_healthy(app->id);
        ESP_LOGI(TAG, "launched %s (%s) in-process", app->name, app->id);
    }
}

static void handle_launch(const launch_req_t *r)
{
    if (r->home) {
        if (frame_wasm_running()) {
            (void)frame_wasm_stop();
        }
        (void)frame_ui_dispatch(go_home, NULL);
        return;
    }

    const frame_app_state_t *app = frame_app_manager_find(r->id);
    if (app == NULL) {
        ESP_LOGW(TAG, "launch of unknown app %s", r->id);
        return;
    }

    switch (app->kind) {
    case FRAME_APP_KIND_BUILTIN:
        (void)frame_ui_dispatch(show_builtin, (void *)app->id);
        break;
    case FRAME_APP_KIND_NATIVE:
        launch_native(app);
        break;
    case FRAME_APP_KIND_MANAGED:
        launch_managed(app);
        break;
    default:
        break;
    }
}

static void shell_task(void *arg)
{
    (void)arg;
    launch_req_t r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) == pdTRUE) {
            handle_launch(&r);
        }
    }
}

/* ------------------------------------------------------------------ */

esp_err_t frame_shell_open(const char *app_id)
{
    if (app_id == NULL || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    launch_req_t r = { 0 };
    strncpy(r.id, app_id, sizeof(r.id) - 1u);
    return (xQueueSend(s_queue, &r, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

void frame_shell_home(void)
{
    if (s_queue == NULL) {
        return;
    }
    launch_req_t r = { 0 };
    r.home = true;
    (void)xQueueSend(s_queue, &r, 0);
}

/* ------------------------------------------------------------------ */

static void on_event(const frame_event_t *evt, void *user)
{
    (void)user;
    if (evt->type != FRAME_EVT_APP_LAUNCH) {
        return;
    }
    const frame_app_state_t *app = frame_app_manager_at((size_t)evt->arg0);
    if (app == NULL) {
        return;
    }
    (void)frame_shell_open(app->id);
}

esp_err_t frame_shell_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(LAUNCH_QUEUE_LEN, sizeof(launch_req_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        shell_task, "frame_shell", 6144, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = frame_event_subscribe(FRAME_EVT_APP_LAUNCH, on_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* If we were restarted by a native app handing control back, there is a
     * handoff record to clear; the shell is now the running image. */
    if (frame_ota_launched_by_shell(NULL, 0)) {
        ESP_LOGI(TAG, "returned from a native app");
        frame_ota_set_boot_shell();
        frame_ota_clear_handoff();
    }
    return ESP_OK;
}
