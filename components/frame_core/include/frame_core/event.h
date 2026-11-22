/*
 * frame_core/event.h — the system event bus.
 *
 * Producers post; subscribers are invoked from frame_event_poll(), which runs
 * on the LVGL task. Nothing here is ISR-safe by design: posting from an ISR is
 * a bug, and the API makes that awkward on purpose.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_EVENT_TEXT_MAX 32u

typedef enum {
    FRAME_EVT_NONE = 0,
    FRAME_EVT_APP_LAUNCH,        /* arg0 = app index                  */
    FRAME_EVT_APP_EXIT,
    FRAME_EVT_APP_ERROR,         /* text = reason                     */
    FRAME_EVT_NETWORK_UP,
    FRAME_EVT_NETWORK_DOWN,
    FRAME_EVT_STORE_UPDATED,     /* arg0 = number of pending updates  */
    FRAME_EVT_APP_INSTALLED,     /* text = app id                     */
    FRAME_EVT_APP_REMOVED,       /* text = app id                     */
    FRAME_EVT_SETTINGS_CHANGED,  /* text = changed key                */
    FRAME_EVT_POWER_IDLE,
    FRAME_EVT_POWER_WAKE,
    FRAME_EVT_SYSTEM_SHUTDOWN,
    FRAME_EVT_RENDER_STATS,      /* periodic stats broadcast          */
    FRAME_EVT_UI_DISPATCH,       /* a closure is queued for the LVGL task */
    FRAME_EVT_TYPE_COUNT
} frame_event_type_t;

typedef struct {
    frame_event_type_t type;
    int32_t             arg0;
    int32_t             arg1;
    char                text[FRAME_EVENT_TEXT_MAX];
} frame_event_t;

typedef void (*frame_event_cb_t)(const frame_event_t *evt, void *user);

#define FRAME_EVENT_QUEUE_LEN 16u
#define FRAME_EVENT_MAX_SUBS  16u

/* Returns ESP_ERR_NO_MEM when the queue is full; the event is dropped, never
 * blocking, because a full queue means the UI thread is already behind. */
esp_err_t frame_event_post(const frame_event_t *evt);

esp_err_t frame_event_post_simple(frame_event_type_t type, int32_t arg0, int32_t arg1);

/* Subscribe to one type, or to everything when type is FRAME_EVT_NONE. */
esp_err_t frame_event_subscribe(frame_event_type_t type, frame_event_cb_t cb, void *user);

/* Drains pending events and dispatches them. Call from the LVGL task. */
void frame_event_poll(uint32_t max_wait_ms);

/* Drops everything pending. Used on shutdown. */
void frame_event_flush(void);

size_t frame_event_pending(void);

#ifdef __cplusplus
}
#endif
