/*
 * frame_core/watchdog.h — liveness monitoring and boot-loop recovery.
 *
 * The task watchdog guards against a stuck LVGL task. The consecutive-failure
 * counter in NVS guards against a boot loop: after three bad boots in a row
 * the device starts in safe mode instead of endlessly crashing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_BOOT_FAILURE_LIMIT 3u

typedef struct {
    uint32_t boot_failures;       /* consecutive, reset on a healthy boot */
    bool     safe_mode;
    uint32_t lvgl_feed_count;
    uint32_t watchdog_resets;
} frame_watchdog_stats_t;

esp_err_t frame_watchdog_init(void);

/* The LVGL task calls this once per frame. Missing the window triggers a reset. */
void frame_watchdog_feed(void);

/* Records that the current boot reached a healthy state, clearing the
 * consecutive-failure counter. */
void frame_watchdog_mark_boot_ok(void);

bool frame_watchdog_safe_mode(void);
void frame_watchdog_get_stats(frame_watchdog_stats_t *out);

#ifdef __cplusplus
}
#endif
