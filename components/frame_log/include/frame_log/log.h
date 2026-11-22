/*
 * frame_log/log.h — PSRAM ring-buffer logger with UART and HTTP sinks.
 *
 * The ring buffer lives in PSRAM so it can hold a few thousand lines without
 * touching the scarce internal SRAM. Writes never block: if the ring is full
 * the oldest line is dropped, because losing an old line is always better than
 * stalling a real-time task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_LOG_LINE_MAX 128u
#define FRAME_LOG_TAG_MAX  16u

typedef enum {
    FRAME_LOG_ERROR = 0,
    FRAME_LOG_WARN,
    FRAME_LOG_INFO,
    FRAME_LOG_DEBUG,
} frame_log_level_t;

typedef struct {
    uint64_t        timestamp_us;
    frame_log_level_t level;
    char            tag[FRAME_LOG_TAG_MAX];
    char            text[FRAME_LOG_LINE_MAX];
} frame_log_line_t;

typedef struct {
    uint32_t ring_bytes;
    uint32_t line_count;
    uint32_t dropped;
    uint32_t total_written;
} frame_log_stats_t;

/* `ring_bytes` is allocated from PSRAM. 0 selects a default of 256 KB. */
esp_err_t frame_log_init(size_t ring_bytes);

void frame_log(frame_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/*
 * printf-style shorthands. Named with a single letter (not FRAME_LOG_INFO,
 * which is the enumerator) so the two can never be confused by the compiler.
 */
#define FRAME_LOGE(tag, fmt, ...) frame_log(FRAME_LOG_ERROR, (tag), (fmt), ##__VA_ARGS__)
#define FRAME_LOGW(tag, fmt, ...) frame_log(FRAME_LOG_WARN,  (tag), (fmt), ##__VA_ARGS__)
#define FRAME_LOGI(tag, fmt, ...) frame_log(FRAME_LOG_INFO,  (tag), (fmt), ##__VA_ARGS__)
#define FRAME_LOGD(tag, fmt, ...) frame_log(FRAME_LOG_DEBUG, (tag), (fmt), ##__VA_ARGS__)

void frame_log_set_level(frame_log_level_t level);
frame_log_level_t frame_log_get_level(void);

/* Iterates the ring from oldest to newest. Returns the number copied. */
size_t frame_log_snapshot(frame_log_line_t *out, size_t max_lines);

/* Newest-first, with level and optional tag filtering. */
size_t frame_log_query(frame_log_level_t min_level, const char *tag,
                       frame_log_line_t *out, size_t max_lines);

void frame_log_clear(void);
void frame_log_get_stats(frame_log_stats_t *out);

#ifdef __cplusplus
}
#endif
