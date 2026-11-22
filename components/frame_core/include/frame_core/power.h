/*
 * frame_core/power.h — the idle ladder.
 *
 * 100% -> 60% after 60 s -> 10% after 180 s -> panel off after 600 s.
 * Any input returns to full brightness immediately.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t dim_after_ms;
    uint32_t idle_after_ms;
    uint32_t off_after_ms;
    uint8_t  dim_level_pct;
    uint8_t  idle_level_pct;
} frame_power_policy_t;

#define FRAME_POWER_POLICY_DEFAULT \
    { .dim_after_ms = 60000u, .idle_after_ms = 180000u, .off_after_ms = 600000u, \
      .dim_level_pct = 60u, .idle_level_pct = 10u }

typedef enum {
    FRAME_POWER_ACTIVE = 0,
    FRAME_POWER_DIM,
    FRAME_POWER_IDLE,
    FRAME_POWER_OFF,
} frame_power_state_t;

esp_err_t frame_power_init(const frame_power_policy_t *policy);

/* Call on any user activity. */
void frame_power_activity(void);

/*
 * Brightness used whenever the device is ACTIVE (user setting, 1..100). Dim
 * and idle levels are derived from it so a user who likes 40% gets 24%/6%
 * rather than being blown back to 100% on every touch.
 */
void      frame_power_set_active_level(uint8_t percent);
uint8_t   frame_power_active_level(void);

/* Called once per LVGL tick; drives the ladder. */
void frame_power_tick(uint32_t now_ms);

/* Replaces the timeouts at runtime (Settings). Keeps the current brightness. */
esp_err_t frame_power_set_policy(const frame_power_policy_t *policy);
const frame_power_policy_t *frame_power_policy(void);

frame_power_state_t frame_power_state(void);
bool                 frame_power_is_idle(void);

#ifdef __cplusplus
}
#endif
