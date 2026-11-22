/*
 * frame_input/gt911.h — Goodix GT911 capacitive touch controller.
 *
 * Implemented in-repo rather than pulled from a managed component because this
 * board does not route the INT line, so the driver must be genuinely
 * poll-driven rather than interrupt-driven with a polling fallback.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "frame_input/input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GT911_I2C_ADDR 0x5Du

/* Register map (single-touch readout). */
#define GT911_REG_PRODUCT_ID  0x8140u
#define GT911_REG_STATUS      0x814Eu
#define GT911_REG_POINT1      0x8150u

#define GT911_POINT_BYTES     8u

typedef struct {
    uint8_t  bus;         /* I2C port    */
    uint8_t  addr;
    uint16_t width;
    uint16_t height;
    bool     invert_x;
    bool     invert_y;
    bool     swap_xy;
} gt911_dev_t;

esp_err_t gt911_init(const gt911_dev_t *dev, int8_t sda, int8_t scl, int8_t rst_gpio);
esp_err_t gt911_read(const gt911_dev_t *dev, frame_touch_point_t *pts,
                     uint8_t max, uint8_t *count);
bool      gt911_probe(const gt911_dev_t *dev);

#ifdef __cplusplus
}
#endif
