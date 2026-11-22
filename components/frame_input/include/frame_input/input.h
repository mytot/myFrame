/*
 * frame_input/input.h — touch, normalised into pointer events.
 *
 * The controller is polled from core 0, never from the LVGL render loop. That
 * is not a style preference: on an RGB panel an I2C transaction in the render
 * path is a known cause of visible tearing, because the bus stalls while the
 * panel is mid-scan.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "frame_core/board_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_INPUT_MAX_POINTS 5u

typedef enum {
    FRAME_INPUT_DOWN = 0,
    FRAME_INPUT_MOVE,
    FRAME_INPUT_UP,
    FRAME_INPUT_CANCEL,
} frame_input_type_t;

typedef struct {
    uint8_t  id;        /* tracking id, stable for the lifetime of a contact */
    uint16_t x;         /* display coordinates, already transformed          */
    uint16_t y;
    uint16_t pressure;  /* 0..255 normalised                                */
} frame_touch_point_t;

typedef struct {
    frame_input_type_t  type;
    frame_touch_point_t point;
    uint32_t            timestamp_us;
} frame_input_event_t;

/* ------------------------------------------------------------------ */

esp_err_t frame_input_init(const frame_touch_desc_t *desc);

/* Pumps the controller and queues events. Called once per LVGL frame. */
void frame_input_poll(void);

/* Drains one queued event. Returns false when the queue is empty. */
bool frame_input_next(frame_input_event_t *out);

void frame_input_set_enabled(bool enabled);
bool frame_input_available(void);

#ifdef __cplusplus
}
#endif
