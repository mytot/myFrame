/*
 * frame_display/display.h — the resource-efficient RGB panel driver.
 *
 * The design constraint this driver exists to satisfy: on an 800x480 RGB panel
 * the framebuffer must live in PSRAM, and PSRAM is the slowest, most contended
 * bus in the system. So LVGL renders into small SRAM buffers and only the
 * invalidated rectangle is copied out — PSRAM traffic ends up proportional to
 * what changed, not to the size of the screen.
 *
 * See docs/DISPLAY_DRIVER.md and docs/RENDER_PERFORMANCE.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "frame_core/board_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    frame_display_profile_t profile;
    uint32_t pclk_hz;
    uint16_t refresh_hz;
    bool     has_touch;
} frame_display_info_t;

typedef struct {
    uint32_t frames_rendered;
    uint32_t frames_skipped;      /* flush requested, nothing was dirty     */
    uint32_t frames_dropped;      /* exceeded the frame budget              */
    uint32_t vsync_missed;
    uint32_t dirty_px;            /* invalidated pixels in the last frame   */

    uint32_t render_us;
    uint32_t copy_us;
    uint32_t frame_us_last;
    uint32_t frame_us_max;

    size_t   sram_bytes;
    size_t   psram_bytes;

    uint32_t core0_load_pct;
    uint32_t core1_load_pct;
} frame_display_stats_t;

/* ------------------------------------------------------------------ */

esp_err_t frame_display_init(const frame_display_desc_t *desc);
esp_err_t frame_display_get_info(frame_display_info_t *out);

/* Backlight, 0..100, gamma-corrected. */
esp_err_t frame_display_set_backlight(uint8_t percent);
uint8_t   frame_display_get_backlight(void);

/* Idle policy: throttle the LVGL timer and drop to the brightness floor. */
esp_err_t frame_display_set_idle(bool idle);

void frame_display_get_stats(frame_display_stats_t *out);

/*
 * Starts the LVGL task on core 1. Call after every component that creates
 * LVGL objects (frame_ui, frame_input) has been initialised; after this the
 * only permitted way to touch LVGL is frame_ui_dispatch().
 */
esp_err_t frame_display_start(void);

lv_display_t *frame_display_lv(void);

/* ------------------------------------------------------------------ */
/* Render budget                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    FRAME_RENDER_CLASS_STATIC = 0,
    FRAME_RENDER_CLASS_LIGHT,
    FRAME_RENDER_CLASS_MEDIUM,
    FRAME_RENDER_CLASS_FULL,
} frame_render_class_t;

/* Clamps an app's animation rate to its declared class. Returns the applied cap. */
uint8_t frame_display_clamp_fps(frame_render_class_t cls, uint8_t requested);

#ifdef __cplusplus
}
#endif
