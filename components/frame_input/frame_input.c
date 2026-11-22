#include "frame_input/input.h"

#include "frame_input/gt911.h"

#include "frame_core/power.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "input";

#define INPUT_QUEUE_LEN 32u

static const frame_touch_desc_t *s_desc;
static gt911_dev_t                s_gt;
static QueueHandle_t              s_queue;
static bool                       s_enabled = true;
static bool                       s_available;

/* Previous frame, used to synthesise DOWN/MOVE/UP transitions. */
static frame_touch_point_t s_prev[FRAME_INPUT_MAX_POINTS];
static uint8_t             s_prev_count;

static void post(frame_input_type_t type, const frame_touch_point_t *p)
{
    /* Any contact is user activity: wake the panel and restart the idle
     * ladder before the event even reaches LVGL. */
    frame_power_activity();

    frame_input_event_t e = {
        .type = type,
        .point = *p,
        .timestamp_us = (uint32_t)esp_timer_get_time(),
    };
    if (xQueueSend(s_queue, &e, 0) != pdTRUE) {
        /* A full queue means the UI thread is behind; dropping a MOVE is
         * harmless, dropping an UP is not. Prefer to keep the UP. */
        if (type == FRAME_INPUT_UP) {
            frame_input_event_t discard;
            xQueueReceive(s_queue, &discard, 0);
            xQueueSend(s_queue, &e, 0);
        }
    }
}

static void diff_frame(const frame_touch_point_t *now, uint8_t now_count)
{
    /* UP for contacts that have gone. */
    for (uint8_t i = 0; i < s_prev_count; i++) {
        bool still = false;
        for (uint8_t j = 0; j < now_count; j++) {
            if (now[j].id == s_prev[i].id) {
                still = true;
                break;
            }
        }
        if (!still) {
            frame_touch_point_t p = s_prev[i];
            post(FRAME_INPUT_UP, &p);
        }
    }

    /* DOWN for new contacts, MOVE for existing ones. */
    for (uint8_t j = 0; j < now_count; j++) {
        bool existed = false;
        for (uint8_t i = 0; i < s_prev_count; i++) {
            if (s_prev[i].id == now[j].id) {
                existed = true;
                if (s_prev[i].x != now[j].x || s_prev[i].y != now[j].y) {
                    post(FRAME_INPUT_MOVE, &now[j]);
                }
                break;
            }
        }
        if (!existed) {
            post(FRAME_INPUT_DOWN, &now[j]);
        }
    }

    memcpy(s_prev, now, sizeof(frame_touch_point_t) * now_count);
    s_prev_count = now_count;
}

/*
 * Core 0, priority 5. Deliberately NOT the LVGL task: an I2C transaction inside
 * the render loop stalls the bus while the panel is mid-scan, which shows up as
 * tearing on RGB panels.
 */
static void input_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000u / (s_desc->poll_hz ? s_desc->poll_hz : 20u));

    ESP_LOGI(TAG, "touch poll task on core %d at %u Hz",
             (int)xPortGetCoreID(), (unsigned)s_desc->poll_hz);

    for (;;) {
        if (s_enabled && s_available) {
            frame_touch_point_t pts[FRAME_INPUT_MAX_POINTS];
            uint8_t n = 0;
            if (gt911_read(&s_gt, pts, FRAME_INPUT_MAX_POINTS, &n) == ESP_OK) {
                diff_frame(pts, n);
            }
        }
        vTaskDelay(period);
    }
}

/* ------------------------------------------------------------------ */
/* LVGL indev — runs on the LVGL task                                   */
/* ------------------------------------------------------------------ */

static void lv_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    frame_input_event_t e;
    if (!xQueueReceive(s_queue, &e, 0)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->point.x = e.point.x;
    data->point.y = e.point.y;

    switch (e.type) {
    case FRAME_INPUT_DOWN:
    case FRAME_INPUT_MOVE:
        data->state = LV_INDEV_STATE_PRESSED;
        break;
    case FRAME_INPUT_UP:
    case FRAME_INPUT_CANCEL:
    default:
        data->state = LV_INDEV_STATE_RELEASED;
        break;
    }
    /* Keep reading: LVGL asks again on the next timer tick. */
    if (uxQueueMessagesWaiting(s_queue) > 0) {
        data->continue_reading = true;
    }
}

/* ------------------------------------------------------------------ */

esp_err_t frame_input_init(const frame_touch_desc_t *desc)
{
    if (desc == NULL) {
        ESP_LOGW(TAG, "board declares no touch controller");
        return ESP_OK;
    }
    s_desc = desc;

    s_queue = xQueueCreate(INPUT_QUEUE_LEN, sizeof(frame_input_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_gt.bus      = (uint8_t)desc->i2c_port;
    s_gt.addr     = desc->i2c_addr;
    s_gt.width    = desc->width;
    s_gt.height   = desc->height;
    s_gt.invert_x = desc->invert_x;
    s_gt.invert_y = desc->invert_y;
    s_gt.swap_xy  = desc->swap_xy;

    esp_err_t err = ESP_OK;
    switch (desc->controller) {
    case FRAME_TOUCH_GT911:
        err = gt911_init(&s_gt, desc->sda_gpio, desc->scl_gpio, desc->rst_gpio);
        if (err == ESP_OK) {
            s_available = gt911_probe(&s_gt);
        }
        break;
    case FRAME_TOUCH_NONE:
        s_available = false;
        break;
    default:
        ESP_LOGW(TAG, "unsupported touch controller %d", (int)desc->controller);
        s_available = false;
        break;
    }

    if (err != ESP_OK) {
        /* Touch is not fatal: the device can still be driven by the store API. */
        ESP_LOGW(TAG, "touch init failed (%s) — input disabled", esp_err_to_name(err));
        s_available = false;
    }

    if (s_available) {
        BaseType_t ok = xTaskCreatePinnedToCore(input_task, "frame_touch", 3072, NULL, 5, NULL, 0);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "cannot create touch task");
            s_available = false;
        }
    }

    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lv_indev_read_cb);

    ESP_LOGI(TAG, "touch %s", s_available ? "active" : "unavailable");
    return ESP_OK;
}

void frame_input_poll(void)
{
    /* LVGL pulls events through the indev read callback on its own timer, so
     * there is nothing to drive here. Kept as a stable hook for the boot
     * sequence and for a future rotary/button input path. */
}

bool frame_input_next(frame_input_event_t *out)
{
    if (out == NULL || s_queue == NULL) {
        return false;
    }
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

void frame_input_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        /* Drop any in-flight contact so it cannot stick. */
        for (uint8_t i = 0; i < s_prev_count; i++) {
            frame_touch_point_t p = s_prev[i];
            post(FRAME_INPUT_UP, &p);
        }
        s_prev_count = 0;
    }
}

bool frame_input_available(void)
{
    return s_available;
}
