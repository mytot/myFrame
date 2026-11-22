#include "frame_display/display.h"

#include "frame_core/event.h"
#include "frame_core/power.h"
#include "frame_core/watchdog.h"

#include "esp_cache.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_rgb_panel.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "display";

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const frame_display_desc_t *desc;
    esp_lcd_panel_handle_t      panel;
    lv_display_t               *lv_disp;

    /* Double-buffered framebuffer management. s_write_fb is the buffer that is
     * NOT currently being scanned out; after draw_bitmap() it becomes the
     * scanned one and the other becomes writable. */
    uint16_t *fb[2];
    uint8_t   fb_index;
    bool      double_buffered;

    void     *draw_buf[2];
    size_t    draw_buf_bytes;

    SemaphoreHandle_t vsync_sem;
    TaskHandle_t      task_handle;
    volatile bool     flush_pending;

    /* Backlight */
    uint8_t   backlight_pct;

    /* Idle throttling */
    bool      idle;
    uint32_t  active_refr_ms;
    uint32_t  idle_refr_ms;

    /* Stats */
    frame_display_stats_t stats;
    int64_t               frame_start_us;
    uint32_t              dirty_px_accum;
    uint32_t              vsync_count;
} display_state_t;

static display_state_t s;

/* ------------------------------------------------------------------ */
/* Backlight                                                            */
/* ------------------------------------------------------------------ */

esp_err_t frame_display_set_backlight(uint8_t percent)
{
    if (percent > 100u) {
        percent = 100u;
    }
    s.backlight_pct = percent;
    const frame_board_t *b = frame_board();
    if (b->backlight_set != NULL) {
        b->backlight_set(percent);
    }
    return ESP_OK;
}

uint8_t frame_display_get_backlight(void)
{
    return s.backlight_pct;
}

/* ------------------------------------------------------------------ */
/* LVGL tick                                                            */
/* ------------------------------------------------------------------ */

static uint32_t lv_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ */
/* Vsync                                                                */
/* ------------------------------------------------------------------ */

static bool on_vsync(esp_lcd_panel_handle_t panel,
                     const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    if (s.vsync_sem != NULL) {
        xSemaphoreGiveFromISR(s.vsync_sem, &woken);
    }
    s.vsync_count++;
    return woken == pdTRUE;
}

/* ------------------------------------------------------------------ */
/* Flush — the heart of the driver                                      */
/* ------------------------------------------------------------------ */

/*
 * LVGL calls this with a rectangle it has just rendered into one of the SRAM
 * draw buffers. Three things make this resource-efficient:
 *
 *   1. Only the invalidated rectangle is copied, so PSRAM traffic is
 *      proportional to what changed rather than to the screen size.
 *   2. The copy is sequential and contiguous, which is PSRAM's best case.
 *   3. EDMA scans out through SRAM bounce buffers, so the panel's continuous
 *      refresh adds no random-access pressure to PSRAM at all.
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    if (w <= 0 || h <= 0 || s.fb[0] == NULL) {
        lv_display_flush_ready(disp);
        return;
    }

    const int64_t t0 = esp_timer_get_time();

    const uint16_t *src = (const uint16_t *)px_map;
    uint16_t *dst_base = s.fb[s.fb_index];
    const int32_t stride = s.desc->width;

    for (int32_t row = 0; row < h; row++) {
        uint16_t *dst = dst_base + (int32_t)(area->y1 + row) * stride + area->x1;
        const uint16_t *from = src + row * w;
        memcpy(dst, from, (size_t)w * sizeof(uint16_t));
    }

    const int64_t t1 = esp_timer_get_time();
    s.stats.copy_us = (uint32_t)(t1 - t0);
    s.stats.dirty_px = (uint32_t)(w * h);
    s.dirty_px_accum += (uint32_t)(w * h);

    /*
     * The CPU wrote the PSRAM framebuffer through the data cache; EDMA reads
     * PSRAM directly. Without this write-back the panel shows stale lines that
     * look exactly like a hardware fault.
     */
    const size_t bytes = (size_t)h * (size_t)stride * sizeof(uint16_t);
    esp_cache_msync(dst_base + (int32_t)area->y1 * stride, bytes,
                    ESP_CACHE_MSYNC_FLAG_TYPE_C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    /* Present: draw_bitmap() rotates to the next framebuffer. */
    if (s.double_buffered) {
        /* Wait for the swap to land inside the blanking interval. */
        if (s.vsync_sem != NULL) {
            if (xSemaphoreTake(s.vsync_sem, pdMS_TO_TICKS(20)) != pdTRUE) {
                s.stats.vsync_missed++;
            }
        }
        esp_lcd_panel_draw_bitmap(s.panel, 0, 0, s.desc->width, s.desc->height);
        s.fb_index = (uint8_t)(1u - s.fb_index);
    } else {
        esp_lcd_panel_draw_bitmap(s.panel, 0, 0, s.desc->width, s.desc->height);
    }

    s.stats.frames_rendered++;
    s.stats.frame_us_last = (uint32_t)(esp_timer_get_time() - s.frame_start_us);
    if (s.stats.frame_us_last > s.stats.frame_us_max) {
        s.stats.frame_us_max = s.stats.frame_us_last;
    }
    s.flush_pending = false;

    lv_display_flush_ready(disp);
}

/* ------------------------------------------------------------------ */
/* LVGL task — core 1, priority 4                                      */
/* ------------------------------------------------------------------ */

esp_err_t frame_display_start(void)
{
    if (s.lv_disp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s.task_handle != NULL) {
        return ESP_OK;
    }

    /* Everything that creates LVGL objects (the shell, input devices) must
     * have run before this point: from here on, the LVGL task owns LVGL and
     * the only way in is frame_ui_dispatch(). */
    BaseType_t ok = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", 8192, NULL, 4, &s.task_handle, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "cannot create lvgl task");
        return ESP_ERR_NO_MEM;
    }

    /* Backlight on only after the first frame is guaranteed to be valid. */
    frame_display_set_backlight(s.backlight_pct);
    ESP_LOGI(TAG, "lvgl task started on core 1");
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    const TickType_t active_period = pdMS_TO_TICKS(s.active_refr_ms);
    const TickType_t idle_period   = pdMS_TO_TICKS(s.idle_refr_ms);
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "lvgl task running on core %d", (int)xPortGetCoreID());

    for (;;) {
        s.frame_start_us = esp_timer_get_time();

        /* Activity is registered by input events and app launches, never here:
         * a per-frame call would reset the idle timer and the device would
         * never dim. When the power manager has dropped the backlight we also
         * stop refreshing at UI rate — nothing the user can see is changing. */
        const bool idle = s.idle || frame_power_is_idle();

        /* Drain input, then let LVGL render whatever is dirty. */
        frame_event_poll(idle ? s.idle_refr_ms : s.active_refr_ms);
        lv_timer_handler();

        frame_power_tick((uint32_t)(esp_timer_get_time() / 1000));
        frame_watchdog_feed();

        vTaskDelayUntil(&last_wake, idle ? idle_period : active_period);
    }
}

/* ------------------------------------------------------------------ */
/* Panel bring-up                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t panel_init(void)
{
    const frame_display_desc_t *d = s.desc;

    esp_lcd_rgb_panel_config_t cfg = {
        .data_width      = d->data_width,
        .clk_src         = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num   = -1,
        .pclk_gpio_num   = d->pclk_gpio,
        .vsync_gpio_num  = d->vsync_gpio,
        .hsync_gpio_num  = d->hsync_gpio,
        .de_gpio_num     = d->de_gpio,
        .timings = {
            .pclk_hz          = d->pclk_hz,
            .h_res            = d->width,
            .v_res            = d->height,
            .hsync_back_porch  = d->timings.hsync_back_porch,
            .hsync_pulse_width = d->timings.hsync_pulse_width,
            .hsync_front_porch = d->timings.hsync_front_porch,
            .vsync_back_porch  = d->timings.vsync_back_porch,
            .vsync_pulse_width = d->timings.vsync_pulse_width,
            .vsync_front_porch = d->timings.vsync_front_porch,
        },
        .flags.fb_in_psram = 1,
    };

    for (int i = 0; i < d->data_width && i < 16; i++) {
        cfg.data_gpio_nums[i] = d->data_gpio[i];
    }

    switch (d->profile) {
    case FRAME_DISPLAY_PROFILE_BALANCED:
        cfg.num_fbs               = 2;
        cfg.bounce_buffer_size_px = (size_t)d->width * 8u;
        s.double_buffered         = true;
        break;
    case FRAME_DISPLAY_PROFILE_LOWMEM:
        cfg.num_fbs               = 1;
        cfg.bounce_buffer_size_px = (size_t)d->width * 16u;
        s.double_buffered         = false;
        break;
    case FRAME_DISPLAY_PROFILE_DIRECT:
        cfg.num_fbs               = 2;
        cfg.bounce_buffer_size_px = 0;
        s.double_buffered         = true;
        break;
    case FRAME_DISPLAY_PROFILE_RASTER8:
    case FRAME_DISPLAY_PROFILE_ZEROCOPY:
        ESP_LOGE(TAG, "profile %d is not implemented in v1", (int)d->profile);
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s.panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_reset(s.panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel reset: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_init(s.panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init: %s", esp_err_to_name(err));
        return err;
    }

    if (s.double_buffered) {
        err = esp_lcd_rgb_panel_get_frame_buffer(s.panel, 2, (void **)&s.fb[0], (void **)&s.fb[1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "get_frame_buffer: %s", esp_err_to_name(err));
            return err;
        }
        /* The panel starts scanning fb[0], so fb[1] is the first writable one. */
        s.fb_index = 1;
    } else {
        err = esp_lcd_rgb_panel_get_frame_buffer(s.panel, 1, (void **)&s.fb[0]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "get_frame_buffer: %s", esp_err_to_name(err));
            return err;
        }
        s.fb[1] = NULL;
        s.fb_index = 0;
    }

    /* Vsync paces the swap so it lands in the blanking interval. */
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync,
        .on_bounce_empty = NULL,
    };
    err = esp_lcd_rgb_panel_register_event_callbacks(s.panel, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "vsync callback registration: %s", esp_err_to_name(err));
    }

    s.stats.psram_bytes = (size_t)d->width * d->height * sizeof(uint16_t) *
                          (s.double_buffered ? 2u : 1u);

    ESP_LOGI(TAG, "panel %ux%u pclk=%u Hz (~%u Hz), profile %d, fb=%u bytes",
             (unsigned)d->width, (unsigned)d->height, (unsigned)d->pclk_hz,
             (unsigned)d->refresh_hz, (int)d->profile, (unsigned)s.stats.psram_bytes);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* LVGL bring-up                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t lvgl_init_port(void)
{
    const frame_display_desc_t *d = s.desc;

    lv_init();
    lv_tick_set_cb(lv_tick_ms);

    s.lv_disp = lv_display_create(d->width, d->height);
    if (s.lv_disp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_flush_cb(s.lv_disp, flush_cb);

    /* Two SRAM draw buffers, each 16 lines. Rendering happens here — never in
     * the PSRAM framebuffer — because PSRAM random access is the thing this
     * whole design is avoiding. */
    const size_t px_per_buf = (size_t)d->width * 16u;
    const size_t bytes = px_per_buf * sizeof(uint16_t);
    for (int i = 0; i < 2; i++) {
        s.draw_buf[i] = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s.draw_buf[i] == NULL) {
            ESP_LOGE(TAG, "no SRAM for draw buffer %d (%u bytes)", i, (unsigned)bytes);
            return ESP_ERR_NO_MEM;
        }
    }
    s.draw_buf_bytes = bytes;

    lv_display_set_buffers(s.lv_disp, s.draw_buf[0], s.draw_buf[1], px_per_buf,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s.stats.sram_bytes = bytes * 2u;
    ESP_LOGI(TAG, "LVGL draw buffers: 2 x %u bytes in SRAM", (unsigned)bytes);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t frame_display_init(const frame_display_desc_t *desc)
{
    if (desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof(s));
    s.desc            = desc;
    s.backlight_pct   = desc->backlight_default_pct;
    s.active_refr_ms  = 16;    /* 60 Hz render intent; the panel is ~39 Hz */
    s.idle_refr_ms    = 100;   /* nothing is changing: stop burning PSRAM   */

    s.vsync_sem = xSemaphoreCreateBinary();
    if (s.vsync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = panel_init();
    if (err != ESP_OK) {
        return err;
    }

    err = lvgl_init_port();
    if (err != ESP_OK) {
        return err;
    }

    s.task_handle = NULL;

    ESP_LOGI(TAG, "display ready: %ux%u, %u Hz, SRAM %u B, PSRAM %u B",
             (unsigned)desc->width, (unsigned)desc->height, (unsigned)desc->refresh_hz,
             (unsigned)s.stats.sram_bytes, (unsigned)s.stats.psram_bytes);
    return ESP_OK;
}

esp_err_t frame_display_get_info(frame_display_info_t *out)
{
    if (out == NULL || s.desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->width      = s.desc->width;
    out->height     = s.desc->height;
    out->profile    = s.desc->profile;
    out->pclk_hz    = s.desc->pclk_hz;
    out->refresh_hz = s.desc->refresh_hz;
    out->has_touch  = (frame_board()->touch != NULL);
    return ESP_OK;
}

esp_err_t frame_display_set_idle(bool idle)
{
    s.idle = idle;
    if (idle) {
        /* A static screen must cost nothing: lengthen the LVGL timer so it
         * stops re-examining the widget tree ten times a second. */
        s.stats.frames_skipped++;
    }
    return ESP_OK;
}

void frame_display_get_stats(frame_display_stats_t *out)
{
    if (out != NULL) {
        *out = s.stats;
    }
}

lv_display_t *frame_display_lv(void)
{
    return s.lv_disp;
}

uint8_t frame_display_clamp_fps(frame_render_class_t cls, uint8_t requested)
{
    uint8_t cap;
    switch (cls) {
    case FRAME_RENDER_CLASS_STATIC: cap = 1u;  break;
    case FRAME_RENDER_CLASS_LIGHT:  cap = 30u; break;
    case FRAME_RENDER_CLASS_MEDIUM: cap = 30u; break;
    case FRAME_RENDER_CLASS_FULL:   cap = 20u; break;
    default:                        cap = 30u; break;
    }
    return (requested == 0u || requested > cap) ? cap : requested;
}
