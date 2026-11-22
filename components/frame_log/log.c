#include "frame_log/log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "frame";

static frame_log_line_t *s_ring;
static size_t            s_capacity;
static volatile uint32_t s_head;     /* next write index */
static volatile uint32_t s_count;    /* valid lines */
static SemaphoreHandle_t s_lock;
static frame_log_level_t s_level = FRAME_LOG_INFO;

static frame_log_stats_t s_stats;

/* The ring is a power-of-two-free circular buffer: capacity is whatever fits,
 * and the index arithmetic uses a modulo on a size_t. */
static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t frame_log_init(size_t ring_bytes)
{
    if (ring_bytes == 0u) {
        ring_bytes = 256u * 1024u;
    }

    s_capacity = ring_bytes / sizeof(frame_log_line_t);
    if (s_capacity < 16u) {
        s_capacity = 16u;
    }

    /* PSRAM first: the ring is bulk data and internal SRAM is far too scarce. */
    s_ring = (frame_log_line_t *)heap_caps_malloc(
        s_capacity * sizeof(frame_log_line_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        s_ring = (frame_log_line_t *)heap_caps_malloc(
            s_capacity * sizeof(frame_log_line_t), MALLOC_CAP_8BIT);
    }
    if (s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_head  = 0;
    s_count = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.ring_bytes = (uint32_t)(s_capacity * sizeof(frame_log_line_t));

    ESP_LOGI(TAG, "log ring: %u lines, %u bytes",
             (unsigned)s_capacity, (unsigned)s_stats.ring_bytes);
    return ESP_OK;
}

void frame_log(frame_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (s_ring == NULL) {
        return;
    }
    if (level > s_level) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    lock();
    frame_log_line_t *line = &s_ring[s_head];
    line->timestamp_us = (uint64_t)esp_timer_get_time();
    line->level        = level;

    const char *t = (tag != NULL) ? tag : "";
    strncpy(line->tag, t, sizeof(line->tag) - 1u);
    line->tag[sizeof(line->tag) - 1u] = '\0';

    int n = vsnprintf(line->text, sizeof(line->text), fmt, ap);
    va_end(ap);
    if (n < 0) {
        line->text[0] = '\0';
    }

    s_head = (uint32_t)((s_head + 1u) % s_capacity);
    if (s_count < s_capacity) {
        s_count++;
    } else {
        s_stats.dropped++;
    }
    s_stats.total_written++;
    s_stats.line_count = s_count;
    unlock();
}

void frame_log_set_level(frame_log_level_t level)
{
    s_level = level;
}

frame_log_level_t frame_log_get_level(void)
{
    return s_level;
}

size_t frame_log_snapshot(frame_log_line_t *out, size_t max_lines)
{
    if (out == NULL || s_ring == NULL || max_lines == 0u) {
        return 0u;
    }
    lock();
    size_t n = (s_count < max_lines) ? s_count : max_lines;
    uint32_t start = (uint32_t)((s_head + s_capacity - (uint32_t)n) % s_capacity);
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(start + (uint32_t)i) % s_capacity];
    }
    unlock();
    return n;
}

size_t frame_log_query(frame_log_level_t min_level, const char *tag,
                       frame_log_line_t *out, size_t max_lines)
{
    if (out == NULL || s_ring == NULL || max_lines == 0u) {
        return 0u;
    }
    lock();
    size_t written = 0;
    /* Walk backwards so the newest matching lines come first. */
    for (uint32_t i = 0; i < s_count && written < max_lines; i++) {
        uint32_t idx = (uint32_t)((s_head + s_capacity - 1u - i) % s_capacity);
        const frame_log_line_t *l = &s_ring[idx];
        if (l->level > min_level) {
            continue;
        }
        if (tag != NULL && tag[0] != '\0' && strcmp(l->tag, tag) != 0) {
            continue;
        }
        out[written++] = *l;
    }
    unlock();
    return written;
}

void frame_log_clear(void)
{
    lock();
    s_head  = 0;
    s_count = 0;
    s_stats.line_count = 0;
    unlock();
}

void frame_log_get_stats(frame_log_stats_t *out)
{
    if (out != NULL) {
        lock();
        *out = s_stats;
        unlock();
    }
}
