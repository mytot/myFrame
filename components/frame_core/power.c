#include "frame_core/power.h"

#include "frame_core/board_types.h"
#include "frame_core/event.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "power";

static frame_power_policy_t  s_policy = FRAME_POWER_POLICY_DEFAULT;
static frame_power_state_t   s_state  = FRAME_POWER_ACTIVE;
static uint32_t              s_last_activity_ms;
static SemaphoreHandle_t     s_lock;
static bool                  s_initialised;
static uint8_t               s_active_pct = 100u;

/* Policy levels are percentages of the user's active brightness. */
static uint8_t scale(uint8_t policy_pct)
{
    return (uint8_t)(((uint32_t)policy_pct * s_active_pct) / 100u);
}

static void set_state(frame_power_state_t next, uint32_t now_ms)
{
    if (next == s_state) {
        return;
    }
    s_state = next;

    const frame_board_t *b = frame_board();
    switch (next) {
    case FRAME_POWER_ACTIVE:
        if (b->backlight_set) b->backlight_set(s_active_pct);
        if (b->exit_sleep)    b->exit_sleep();
        break;
    case FRAME_POWER_DIM:
        if (b->backlight_set) b->backlight_set(scale(s_policy.dim_level_pct));
        break;
    case FRAME_POWER_IDLE:
        if (b->backlight_set) b->backlight_set(scale(s_policy.idle_level_pct));
        break;
    case FRAME_POWER_OFF:
        if (b->enter_sleep)   b->enter_sleep();
        break;
    }

    frame_event_t e = { .type = (next == FRAME_POWER_ACTIVE) ? FRAME_EVT_POWER_WAKE
                                                             : FRAME_EVT_POWER_IDLE,
                        .arg0 = (int32_t)next,
                        .arg1 = (int32_t)now_ms };
    frame_event_post(&e);

    static const char *names[] = { "active", "dim", "idle", "off" };
    ESP_LOGI(TAG, "power -> %s", names[(int)next]);
}

esp_err_t frame_power_init(const frame_power_policy_t *policy)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (policy != NULL) {
        s_policy = *policy;
    }
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_state = FRAME_POWER_ACTIVE;
    s_initialised = true;

    const frame_board_t *b = frame_board();
    if (b->backlight_set) {
        b->backlight_set(s_active_pct);
    }
    ESP_LOGI(TAG, "idle ladder: dim %us, idle %us, off %us",
             (unsigned)(s_policy.dim_after_ms / 1000u),
             (unsigned)(s_policy.idle_after_ms / 1000u),
             (unsigned)(s_policy.off_after_ms / 1000u));
    return ESP_OK;
}

void frame_power_activity(void)
{
    if (!s_initialised || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    frame_power_state_t prev = s_state;
    xSemaphoreGive(s_lock);

    if (prev != FRAME_POWER_ACTIVE) {
        set_state(FRAME_POWER_ACTIVE, s_last_activity_ms);
    }
}

void frame_power_tick(uint32_t now_ms)
{
    if (!s_initialised || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t idle_ms = now_ms - s_last_activity_ms;
    xSemaphoreGive(s_lock);

    if (idle_ms >= s_policy.off_after_ms) {
        set_state(FRAME_POWER_OFF, now_ms);
    } else if (idle_ms >= s_policy.idle_after_ms) {
        set_state(FRAME_POWER_IDLE, now_ms);
    } else if (idle_ms >= s_policy.dim_after_ms) {
        set_state(FRAME_POWER_DIM, now_ms);
    }
}

frame_power_state_t frame_power_state(void)
{
    return s_state;
}

bool frame_power_is_idle(void)
{
    return s_state >= FRAME_POWER_IDLE;
}

void frame_power_set_active_level(uint8_t percent)
{
    if (percent < 1u) {
        percent = 1u;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    s_active_pct = percent;
    if (s_initialised && s_state == FRAME_POWER_ACTIVE) {
        const frame_board_t *b = frame_board();
        if (b->backlight_set) {
            b->backlight_set(s_active_pct);
        }
    }
}

uint8_t frame_power_active_level(void)
{
    return s_active_pct;
}

esp_err_t frame_power_set_policy(const frame_power_policy_t *policy)
{
    if (policy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialised) {
        return frame_power_init(policy);
    }
    s_policy = *policy;
    /* Re-evaluate immediately so a shorter timeout takes effect now. */
    frame_power_tick((uint32_t)(esp_timer_get_time() / 1000));
    return ESP_OK;
}

const frame_power_policy_t *frame_power_policy(void)
{
    return &s_policy;
}
