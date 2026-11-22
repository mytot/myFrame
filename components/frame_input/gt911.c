#include "frame_input/gt911.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "gt911";

/* ------------------------------------------------------------------ */
/* Bus                                                                  */
/* ------------------------------------------------------------------ */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool                    s_ready;

static esp_err_t bus_create(int8_t sda, int8_t scl, int8_t port, uint8_t addr);

bool gt911_probe(const gt911_dev_t *dev)
{
    (void)dev;
    return s_ready;
}

esp_err_t gt911_init(const gt911_dev_t *dev, int8_t sda, int8_t scl, int8_t rst_gpio)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Reset: hold RST low, then release. The GT911 latches its I2C address from
     * the INT pin during reset; on this board INT is not routed, so the address
     * settles at 0x5D and we must use that — not the 0x14 that most copy-pasted
     * configs assume.
     */
    if (rst_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << rst_gpio,
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(rst_gpio, 0);
        esp_rom_delay_us(20000);
        gpio_set_level(rst_gpio, 1);
        /* The controller needs ~50 ms after reset before it will answer. */
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    return bus_create(sda, scl, (int8_t)dev->bus, dev->addr);
}

static esp_err_t reg_read(uint16_t reg, uint8_t *buf, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFu) };
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, len,
                                       pdMS_TO_TICKS(20));
}

static esp_err_t reg_write(uint16_t reg, uint8_t val)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFu), val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(20));
}

esp_err_t gt911_read(const gt911_dev_t *dev, frame_touch_point_t *pts,
                     uint8_t max, uint8_t *count)
{
    if (dev == NULL || pts == NULL || count == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    uint8_t status = 0;
    esp_err_t err = reg_read(GT911_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }

    /* Bit 7 = data ready. Clearing the status register acknowledges the frame. */
    if ((status & 0x80u) == 0u) {
        return ESP_OK;
    }

    uint8_t n = status & 0x0Fu;
    if (n > max) {
        n = max;
    }
    if (n > FRAME_INPUT_MAX_POINTS) {
        n = FRAME_INPUT_MAX_POINTS;
    }

    if (n > 0u) {
        uint8_t raw[FRAME_INPUT_MAX_POINTS * GT911_POINT_BYTES];
        err = reg_read(GT911_REG_POINT1, raw, (size_t)n * GT911_POINT_BYTES);
        if (err != ESP_OK) {
            return err;
        }

        for (uint8_t i = 0; i < n; i++) {
            const uint8_t *p = &raw[i * GT911_POINT_BYTES];
            uint16_t x = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
            uint16_t y = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));
            uint16_t w = (uint16_t)(p[6] | ((uint16_t)p[7] << 8));

            /* Orient into display coordinates. */
            uint16_t ox = x, oy = y;
            if (dev->swap_xy) {
                uint16_t t = ox; ox = oy; oy = t;
            }
            if (dev->invert_x) ox = (uint16_t)(dev->width - 1u - ox);
            if (dev->invert_y) oy = (uint16_t)(dev->height - 1u - oy);

            /* Palm rejection: a contact wider than ~12 mm is a palm, not a
             * finger. GT911 reports width in units of 0.1 mm at this gain. */
            if (w > 120u) {
                continue;
            }

            pts[*count].id       = p[0];
            pts[*count].x        = ox;
            pts[*count].y        = oy;
            pts[*count].pressure = 255u;
            (*count)++;
        }
    }

    /* Acknowledge so the controller raises the next frame. */
    reg_write(GT911_REG_STATUS, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Bus creation — separated from gt911_init so the board descriptor can be
 * used without a hard dependency on its GPIO numbers in this translation unit.
 * ------------------------------------------------------------------ */

static esp_err_t bus_create(int8_t sda, int8_t scl, int8_t port, uint8_t addr)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (sda < 0 || scl < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port          = (i2c_port_num_t)port,
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;

    /* Confirm the controller is actually there. */
    uint8_t id[4] = {0};
    if (reg_read(GT911_REG_PRODUCT_ID, id, sizeof(id)) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 product id %02x %02x %02x %02x (addr 0x%02X)",
                 id[0], id[1], id[2], id[3], addr);
    } else {
        ESP_LOGW(TAG, "GT911 did not answer at 0x%02X — touch will be inactive", addr);
    }
    return ESP_OK;
}
