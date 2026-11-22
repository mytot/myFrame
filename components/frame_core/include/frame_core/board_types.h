/*
 * frame_core/board_types.h — the contract between FrameOS and a board.
 *
 * A board is data, not a fork. Everything the rest of the system needs to know
 * about the hardware is expressed here; `devices/<board>/board.c` produces one
 * const instance and `main` hands its fields to each subsystem.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Chip                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    FRAME_CHIP_ESP32 = 0,
    FRAME_CHIP_ESP32S2,
    FRAME_CHIP_ESP32S3,
    FRAME_CHIP_ESP32C3,
    FRAME_CHIP_ESP32C6,
    FRAME_CHIP_ESP32P4,
} frame_chip_t;

const char *frame_chip_str(frame_chip_t c);

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

/*
 * Buffer profiles. See docs/DISPLAY_DRIVER.md §2 and
 * docs/RENDER_PERFORMANCE.md for the arithmetic behind each one.
 */
typedef enum {
    /* 2 x 16-bit fb in PSRAM, LVGL PARTIAL into SRAM, SRAM bounce buffers.
     * Default for the 8048S070. ~77 KB SRAM, 1.5 MB PSRAM, 32 MB/s panel. */
    FRAME_DISPLAY_PROFILE_BALANCED = 0,
    /* Single framebuffer. 0.77 MB PSRAM; tearing possible during full-screen
     * animation. For <= 2 MB PSRAM boards. */
    FRAME_DISPLAY_PROFILE_LOWMEM,
    /* 8-bit paletted framebuffer expanded to RGB565 in the bounce callback.
     * Halves both memory and panel traffic; 256 colours; ~6-15% ISR load. */
    FRAME_DISPLAY_PROFILE_RASTER8,
    /* LVGL renders straight into the PSRAM framebuffer. Video/slideshow only. */
    FRAME_DISPLAY_PROFILE_DIRECT,
    /* No framebuffer at all; the bounce callback generates pixels. Small
     * panels only — cannot render LVGL content at 800x480. */
    FRAME_DISPLAY_PROFILE_ZEROCOPY,
} frame_display_profile_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t pclk_hz;

    frame_display_profile_t profile;

    /* Blanking intervals, in pixel-clock / line units. */
    struct {
        uint16_t hsync_pulse_width;
        uint16_t hsync_back_porch;
        uint16_t hsync_front_porch;
        uint16_t vsync_pulse_width;
        uint16_t vsync_back_porch;
        uint16_t vsync_front_porch;
    } timings;

    /* Parallel RGB bus. -1 means "not connected". */
    int8_t  pclk_gpio;
    int8_t  de_gpio;
    int8_t  vsync_gpio;
    int8_t  hsync_gpio;
    int8_t  data_gpio[16];
    uint8_t data_width;             /* 16 for RGB565 */

    /* Backlight. */
    int8_t  backlight_gpio;
    uint16_t backlight_freq_hz;
    bool    backlight_active_high;
    uint8_t  backlight_default_pct;

    /* Derived: frames per second implied by pclk_hz and the timing block. */
    uint16_t refresh_hz;
} frame_display_desc_t;

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    FRAME_TOUCH_NONE = 0,
    FRAME_TOUCH_GT911,
} frame_touch_controller_t;

typedef struct {
    frame_touch_controller_t controller;
    int8_t   i2c_port;
    uint8_t  i2c_addr;
    int8_t   sda_gpio;
    int8_t   scl_gpio;
    int8_t   rst_gpio;      /* -1 when not routed */
    int8_t   int_gpio;      /* -1 when not routed -> polling only */
    uint16_t poll_hz;
    /* Panel orientation relative to natural. */
    bool     invert_x;
    bool     invert_y;
    bool     swap_xy;
    uint16_t width;
    uint16_t height;
} frame_touch_desc_t;

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    FRAME_AUDIO_NONE = 0,
    FRAME_AUDIO_I2S_DAC,
    FRAME_AUDIO_ES8311,
} frame_audio_codec_t;

typedef struct {
    frame_audio_codec_t codec;
    int8_t   bclk_gpio;
    int8_t   lrck_gpio;
    int8_t   dout_gpio;
    int8_t   mclk_gpio;
    uint32_t sample_rate;
} frame_audio_desc_t;

/* ------------------------------------------------------------------ */
/* Board                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char   *id;
    const char   *revision;
    frame_chip_t  chip;
    struct {
        uint32_t flash_mb;
        uint32_t psram_mb;
    } mem;

    const frame_display_desc_t *display;
    const frame_touch_desc_t   *touch;    /* NULL when the board has none */
    const frame_audio_desc_t   *audio;    /* NULL when the board has none */

    /* Bring-up: buses, GPIOs, panel, touch. Called once from app_main. */
    esp_err_t (*init)(void);

    void (*backlight_set)(uint8_t percent);
    void (*enter_sleep)(void);
    void (*exit_sleep)(void);
} frame_board_t;

/*
 * Provided by the board component selected at build time (-DBOARD=<id>).
 * Declared here rather than in the board's own header so that `main` can be
 * written once and linked against whichever board is selected.
 */
const frame_board_t *frame_board(void);

#ifdef __cplusplus
}
#endif
