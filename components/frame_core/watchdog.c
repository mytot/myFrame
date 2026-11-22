#include "frame_core/watchdog.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "wdt";

#define NVS_NS        "frame"
#define NVK_BOOT_FAIL "boot_fail"
#define NVK_SAFE_MODE "safe_mode"

static frame_watchdog_stats_t s_stats;
static bool                   s_safe_mode;
static int64_t                s_last_feed_us;
static bool                   s_started;

static esp_err_t nvs_open_ro(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READONLY, h);
}

static esp_err_t nvs_open_rw(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READWRITE, h);
}

esp_err_t frame_watchdog_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));

    /* Read the persisted failure counter before anything can reset it. */
    nvs_handle_t h;
    if (nvs_open_ro(&h) == ESP_OK) {
        uint8_t fails = 0;
        uint8_t safe  = 0;
        if (nvs_get_u8(h, NVK_BOOT_FAIL, &fails) == ESP_OK) {
            s_stats.boot_failures = fails;
        }
        if (nvs_get_u8(h, NVK_SAFE_MODE, &safe) == ESP_OK) {
            s_safe_mode = (safe != 0u);
        }
        nvs_close(h);
    }

    /* A boot only counts as a failure if the previous one never reached a
     * healthy state. Increment now; frame_watchdog_mark_boot_ok() resets it. */
    s_stats.boot_failures++;
    if (s_stats.boot_failures >= FRAME_BOOT_FAILURE_LIMIT) {
        s_safe_mode = true;
        ESP_LOGW(TAG, "boot failure limit reached (%u) — entering safe mode",
                 (unsigned)s_stats.boot_failures);
    }

    if (nvs_open_rw(&h) == ESP_OK) {
        nvs_set_u8(h, NVK_BOOT_FAIL, (uint8_t)s_stats.boot_failures);
        nvs_set_u8(h, NVK_SAFE_MODE, s_safe_mode ? 1u : 0u);
        nvs_commit(h);
        nvs_close(h);
    }

    if (!s_safe_mode) {
        /* Watch the LVGL task only: it is the one whose stall the user can see. */
        esp_err_t err = esp_task_wdt_init(
            &(esp_task_wdt_config_t){
                .timeout_ms = 5000,
                .idle_core_mask = 0,
                .trigger_panic = true,
            },
            NULL);
        if (err == ESP_OK) {
            s_started = true;
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "task watchdog init: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "safe mode: watchdog disabled, automatic app launch suppressed");
    }

    s_last_feed_us = esp_timer_get_time();
    ESP_LOGI(TAG, "boot failure count %u, safe mode %d",
             (unsigned)s_stats.boot_failures, s_safe_mode ? 1 : 0);
    return ESP_OK;
}

void frame_watchdog_feed(void)
{
    s_last_feed_us = esp_timer_get_time();
    if (s_started) {
        esp_task_wdt_reset();
    }
}

void frame_watchdog_mark_boot_ok(void)
{
    s_stats.boot_failures = 0;

    nvs_handle_t h;
    if (nvs_open_rw(&h) == ESP_OK) {
        nvs_set_u8(h, NVK_BOOT_FAIL, 0);
        /* A healthy boot clears safe mode only if it was caused by failures,
         * not if the user selected it explicitly. */
        nvs_set_u8(h, NVK_SAFE_MODE, 0);
        nvs_commit(h);
        nvs_close(h);
    }
    s_safe_mode = false;
    ESP_LOGI(TAG, "boot marked healthy");
}

bool frame_watchdog_safe_mode(void)
{
    return s_safe_mode;
}

void frame_watchdog_get_stats(frame_watchdog_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}
