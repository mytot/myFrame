/*
 * frame_pure/slot_table.h — native app slot allocation state machine.
 *
 * Slots are OTA partitions that hold native app images. The table is the
 * authority on which slot holds what; it is persisted to NVS and every
 * operation is designed so that a power cut at any point leaves a *reclaimable*
 * slot rather than a broken one.
 *
 * Pure logic: the caller supplies a monotonic millisecond clock, which makes
 * reservation expiry testable on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FRAME_OTA_MAX_SLOTS
#define FRAME_OTA_MAX_SLOTS 6u
#endif

#define FRAME_SLOT_ID_MAX   64u
#define FRAME_SLOT_VER_MAX  24u

typedef enum {
    FRAME_SLOT_FREE = 0,   /* erased and available                       */
    FRAME_SLOT_RESERVED,   /* claimed by an in-flight install (has expiry) */
    FRAME_SLOT_VALID,      /* committed image, referenced by the registry */
    FRAME_SLOT_STALE,      /* no registry reference — reclaimable         */
} frame_slot_state_t;

typedef struct {
    frame_slot_state_t state;
    char   app_id[FRAME_SLOT_ID_MAX];
    char   version[FRAME_SLOT_VER_MAX];
    uint32_t reserved_at_ms;
    uint32_t last_launched_ms;   /* for LRU eviction */
    uint32_t install_count;
} frame_slot_t;

typedef struct {
    frame_slot_t slots[FRAME_OTA_MAX_SLOTS];
    uint8_t      count;
} frame_slot_table_t;

/* ------------------------------------------------------------------ */

void frame_slot_table_init(frame_slot_table_t *t, uint8_t count);

/* Returns the slot index, or -1 when the table is full. */
int frame_slot_table_reserve(frame_slot_table_t *t, uint32_t now_ms);

/* Marks a reserved slot as holding a committed image. */
int frame_slot_table_commit(frame_slot_table_t *t, uint8_t idx,
                            const char *app_id, const char *version);

/* Releases a slot back to FREE (used when an install fails). */
void frame_slot_table_release(frame_slot_table_t *t, uint8_t idx);

/* Finds the slot currently holding app_id, or -1. */
int frame_slot_table_find(const frame_slot_table_t *t, const char *app_id);

/* Records a launch for LRU purposes. */
void frame_slot_table_touch(frame_slot_table_t *t, uint8_t idx, uint32_t now_ms);

/*
 * Reclaims reservations older than expiry_ms and demotes VALID slots that no
 * longer have a registry reference. `is_referenced` may be NULL, in which case
 * no VALID slot is demoted.
 */
void frame_slot_table_sweep(frame_slot_table_t *t, uint32_t now_ms, uint32_t expiry_ms,
                            bool (*is_referenced)(const char *app_id, void *ctx), void *ctx);

/*
 * Picks a victim for eviction: the least-recently-launched VALID slot whose
 * app id is not `protect_id`. Returns a slot index or -1.
 */
int frame_slot_table_evict_lru(const frame_slot_table_t *t, const char *protect_id);

/* Number of slots in each state; any out pointer may be NULL. */
void frame_slot_table_counts(const frame_slot_table_t *t,
                             uint8_t *free_out, uint8_t *used_out);

#ifdef __cplusplus
}
#endif
