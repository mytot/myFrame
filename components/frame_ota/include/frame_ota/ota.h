/*
 * frame_ota/ota.h — native app slots and boot selection.
 *
 * Native apps live in real OTA partitions (ota_2..ota_5), not a custom
 * partition type. That means the stock bootloader already implements "pick the
 * slot with the highest sequence number, roll back if the new image never
 * reports healthy" — so an app that crashes falls back to the shell for free.
 *
 * Nothing outside this component may write to an app partition or change the
 * boot partition. That is what keeps a bad app from bricking the device.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#include "frame_core/app_manager.h"
#include "frame_pure/slot_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_OTA_CHUNK_ERASE  (64u * 1024u)
#define FRAME_OTA_CHUNK_WRITE  (8u * 1024u)

typedef struct {
    frame_slot_table_t table;
    uint32_t           erase_us;
    uint32_t           write_us;
    uint32_t           bytes_written;
} frame_ota_stats_t;

esp_err_t frame_ota_init(void);
esp_err_t frame_ota_load(void);
esp_err_t frame_ota_save(void);

/* Returns a slot index or -1 when the table is full. */
int  frame_ota_reserve_slot(void);
void frame_ota_release_slot(int slot);
esp_err_t frame_ota_commit_slot(int slot, const char *app_id, const char *version);
int  frame_ota_find_slot(const char *app_id);
void frame_ota_touch_slot(int slot);
void frame_ota_sweep(uint32_t expiry_ms,
                     bool (*is_referenced)(const char *app_id, void *ctx), void *ctx);
int  frame_ota_evict_lru(const char *protect_id);
uint32_t frame_ota_now_ms(void);

/* Erases a slot in bounded chunks so the UI never stalls for long. */
esp_err_t frame_ota_erase_slot(int slot);

const esp_partition_t *frame_ota_slot_partition(int slot);

/* ------------------------------------------------------------------ */
/* Image write                                                           */
/* ------------------------------------------------------------------ */

typedef struct frame_ota_writer frame_ota_writer_t;

esp_err_t frame_ota_write_begin(int slot, size_t image_size, frame_ota_writer_t **out);
esp_err_t frame_ota_write_chunk(frame_ota_writer_t *w, const void *data, size_t len);
/* Validates the image, commits the slot, updates the registry. */
esp_err_t frame_ota_write_end(frame_ota_writer_t *w, const char *app_id, const char *version);
void      frame_ota_write_abort(frame_ota_writer_t *w);

/* ------------------------------------------------------------------ */
/* Boot selection                                                        */
/* ------------------------------------------------------------------ */

/*
 * Records the handoff and sets the boot partition so the next restart lands in
 * the named app. Returns the slot index or -1.
 */
int  frame_ota_set_boot_app(const char *app_id);

/* Points the boot partition back at the shell and clears the handoff. */
esp_err_t frame_ota_set_boot_shell(void);

/* True when this image was launched by the shell rather than by a power cycle. */
bool frame_ota_launched_by_shell(char *app_id_out, size_t cap);

void frame_ota_clear_handoff(void);

/* Rollback support (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). A freshly written
 * image boots as PENDING_VERIFY; if it never confirms, the bootloader reverts
 * to the previous slot on the next reset. The shell confirms once the first
 * frames have rendered; a native app confirms through frame_app_mark_healthy(). */
bool      frame_ota_boot_pending(void);
esp_err_t frame_ota_confirm_boot(void);
void frame_ota_get_stats(frame_ota_stats_t *out);
const frame_slot_table_t *frame_ota_table(void);

#ifdef __cplusplus
}
#endif
