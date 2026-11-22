#include "frame_pure/slot_table.h"

#include <string.h>

void frame_slot_table_init(frame_slot_table_t *t, uint8_t count)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->count = (count > FRAME_OTA_MAX_SLOTS) ? FRAME_OTA_MAX_SLOTS : count;
    for (uint8_t i = 0; i < t->count; i++) {
        t->slots[i].state = FRAME_SLOT_FREE;
    }
}

int frame_slot_table_reserve(frame_slot_table_t *t, uint32_t now_ms)
{
    if (t == NULL) {
        return -1;
    }
    for (uint8_t i = 0; i < t->count; i++) {
        if (t->slots[i].state == FRAME_SLOT_FREE) {
            t->slots[i].state          = FRAME_SLOT_RESERVED;
            t->slots[i].reserved_at_ms = now_ms;
            t->slots[i].app_id[0]      = '\0';
            t->slots[i].version[0]     = '\0';
            return (int)i;
        }
    }
    return -1;
}

int frame_slot_table_commit(frame_slot_table_t *t, uint8_t idx,
                            const char *app_id, const char *version)
{
    if (t == NULL || idx >= t->count || app_id == NULL || version == NULL) {
        return -1;
    }
    if (t->slots[idx].state != FRAME_SLOT_RESERVED) {
        return -1; /* only a reserved slot may be committed */
    }
    if (strlen(app_id) >= FRAME_SLOT_ID_MAX || strlen(version) >= FRAME_SLOT_VER_MAX) {
        return -1;
    }
    memcpy(t->slots[idx].app_id, app_id, strlen(app_id) + 1u);
    memcpy(t->slots[idx].version, version, strlen(version) + 1u);
    t->slots[idx].state = FRAME_SLOT_VALID;
    t->slots[idx].install_count++;
    return 0;
}

void frame_slot_table_release(frame_slot_table_t *t, uint8_t idx)
{
    if (t == NULL || idx >= t->count) {
        return;
    }
    if (t->slots[idx].state != FRAME_SLOT_RESERVED) {
        return; /* never release a committed image through this path */
    }
    memset(&t->slots[idx], 0, sizeof(t->slots[idx]));
    t->slots[idx].state = FRAME_SLOT_FREE;
}

int frame_slot_table_find(const frame_slot_table_t *t, const char *app_id)
{
    if (t == NULL || app_id == NULL) {
        return -1;
    }
    for (uint8_t i = 0; i < t->count; i++) {
        if (t->slots[i].state == FRAME_SLOT_VALID &&
            strcmp(t->slots[i].app_id, app_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void frame_slot_table_touch(frame_slot_table_t *t, uint8_t idx, uint32_t now_ms)
{
    if (t == NULL || idx >= t->count) {
        return;
    }
    t->slots[idx].last_launched_ms = now_ms;
}

void frame_slot_table_sweep(frame_slot_table_t *t, uint32_t now_ms, uint32_t expiry_ms,
                            bool (*is_referenced)(const char *app_id, void *ctx), void *ctx)
{
    if (t == NULL) {
        return;
    }
    for (uint8_t i = 0; i < t->count; i++) {
        frame_slot_t *s = &t->slots[i];

        if (s->state == FRAME_SLOT_RESERVED) {
            /* Clock-safe subtraction: correct across a 32-bit wrap. */
            uint32_t age = now_ms - s->reserved_at_ms;
            if (age >= expiry_ms) {
                memset(s, 0, sizeof(*s));
                s->state = FRAME_SLOT_FREE;
            }
            continue;
        }

        if (s->state == FRAME_SLOT_VALID && is_referenced != NULL) {
            if (!is_referenced(s->app_id, ctx)) {
                s->state = FRAME_SLOT_STALE;
            }
        }
    }
}

int frame_slot_table_evict_lru(const frame_slot_table_t *t, const char *protect_id)
{
    if (t == NULL) {
        return -1;
    }
    int best = -1;
    uint32_t best_age = 0;

    for (uint8_t i = 0; i < t->count; i++) {
        const frame_slot_t *s = &t->slots[i];
        if (s->state != FRAME_SLOT_VALID) {
            continue;
        }
        if (protect_id != NULL && strcmp(s->app_id, protect_id) == 0) {
            continue;
        }
        /* Never evict the shell: it is never in this table, but be explicit. */
        if (strcmp(s->app_id, "myframe.shell") == 0) {
            continue;
        }
        if (best < 0 || s->last_launched_ms < best_age) {
            best     = (int)i;
            best_age = s->last_launched_ms;
        }
    }
    return best;
}

void frame_slot_table_counts(const frame_slot_table_t *t, uint8_t *free_out, uint8_t *used_out)
{
    uint8_t fr = 0, us = 0;
    if (t != NULL) {
        for (uint8_t i = 0; i < t->count; i++) {
            if (t->slots[i].state == FRAME_SLOT_FREE) {
                fr++;
            } else {
                us++;
            }
        }
    }
    if (free_out != NULL) {
        *free_out = fr;
    }
    if (used_out != NULL) {
        *used_out = us;
    }
}
