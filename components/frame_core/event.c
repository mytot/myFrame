#include "frame_core/event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

typedef struct {
    frame_event_type_t type;   /* FRAME_EVT_NONE means "all types" */
    frame_event_cb_t   cb;
    void              *user;
} subscriber_t;

static QueueHandle_t   s_queue;
static subscriber_t    s_subs[FRAME_EVENT_MAX_SUBS];
static uint8_t         s_sub_count;

esp_err_t frame_event_post(const frame_event_t *evt)
{
    if (s_queue == NULL || evt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* xQueueSend with zero timeout: never block the caller. A dropped event is
     * recoverable; a blocked producer is not. */
    if (xQueueSend(s_queue, evt, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t frame_event_post_simple(frame_event_type_t type, int32_t arg0, int32_t arg1)
{
    frame_event_t e = { .type = type, .arg0 = arg0, .arg1 = arg1, .text = {0} };
    return frame_event_post(&e);
}

esp_err_t frame_event_subscribe(frame_event_type_t type, frame_event_cb_t cb, void *user)
{
    if (s_queue == NULL) {
        /* Lazy init keeps the call order forgiving for early subscribers. */
        s_queue = xQueueCreate(FRAME_EVENT_QUEUE_LEN, sizeof(frame_event_t));
        if (s_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sub_count >= FRAME_EVENT_MAX_SUBS) {
        return ESP_ERR_NO_MEM;
    }
    s_subs[s_sub_count].type = type;
    s_subs[s_sub_count].cb   = cb;
    s_subs[s_sub_count].user = user;
    s_sub_count++;
    return ESP_OK;
}

void frame_event_poll(uint32_t max_wait_ms)
{
    if (s_queue == NULL) {
        return;
    }
    frame_event_t e;
    TickType_t deadline = (max_wait_ms == 0u) ? 0u : pdMS_TO_TICKS(max_wait_ms);

    while (xQueueReceive(s_queue, &e, deadline) == pdTRUE) {
        for (uint8_t i = 0; i < s_sub_count; i++) {
            if (s_subs[i].type == FRAME_EVT_NONE || s_subs[i].type == e.type) {
                s_subs[i].cb(&e, s_subs[i].user);
            }
        }
        /* Only block on the first receive; drain whatever is already queued. */
        deadline = 0;
    }
}

void frame_event_flush(void)
{
    if (s_queue == NULL) {
        return;
    }
    frame_event_t e;
    while (xQueueReceive(s_queue, &e, 0) == pdTRUE) {
        /* discard */
    }
}

size_t frame_event_pending(void)
{
    return (s_queue != NULL) ? uxQueueMessagesWaiting(s_queue) : 0u;
}
