/*
 * frame_core/app_manager.h — the list of apps the launcher shows.
 *
 * Holds built-in apps plus whatever the registry reports as installed, and
 * computes the update flags the launcher renders as badges.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_APP_ID_MAX   64u
#define FRAME_APP_NAME_MAX 48u
#define FRAME_APP_VER_MAX  24u
#define FRAME_APP_PATH_MAX 96u
#define FRAME_APP_MAX      32u

typedef enum {
    FRAME_APP_KIND_BUILTIN = 0,
    FRAME_APP_KIND_NATIVE,     /* installed firmware image in a slot */
    FRAME_APP_KIND_MANAGED,    /* WASM module in the FAT filesystem   */
} frame_app_kind_t;

#define FRAME_APP_NO_SLOT 0xFFu

typedef struct {
    char              id[FRAME_APP_ID_MAX];
    char              name[FRAME_APP_NAME_MAX];
    char              version[FRAME_APP_VER_MAX];
    char              available_version[FRAME_APP_VER_MAX];  /* "" when none */
    frame_app_kind_t  kind;
    uint8_t           slot;              /* FRAME_APP_NO_SLOT for built-ins */
    char              icon_path[FRAME_APP_PATH_MAX];
    bool              update_available;
    bool              builtin;
    bool              healthy;           /* false after a crash on last launch */
    uint32_t          last_launched_ms;  /* for launcher ordering            */
} frame_app_state_t;

/* ------------------------------------------------------------------ */

esp_err_t frame_app_manager_init(void);

size_t frame_app_manager_count(void);
const frame_app_state_t *frame_app_manager_at(size_t index);
const frame_app_state_t *frame_app_manager_find(const char *id);

/* Adds or replaces an entry. Built-ins are inserted first and never removed. */
esp_err_t frame_app_manager_upsert(const frame_app_state_t *state);
esp_err_t frame_app_manager_remove(const char *id);

/* Records the newest catalog version seen for an app and derives the badge. */
esp_err_t frame_app_manager_set_available(const char *id, const char *version);
void      frame_app_manager_clear_available(const char *id);

uint32_t frame_app_manager_update_count(void);

/* Orders built-ins first, then most-recently-launched. */
void frame_app_manager_sort(void);

/* Records a launch so the launcher can order non-builtins by recency. */
void frame_app_manager_mark_launched(const char *id, uint32_t now_ms);

/* Marks an app as having crashed on its last launch. */
void frame_app_manager_mark_unhealthy(const char *id);
void frame_app_manager_mark_healthy(const char *id);

/* Order of built-in tiles on the launcher home page. */
#define FRAME_LAUNCHER_ORDER_STORE    0u
#define FRAME_LAUNCHER_ORDER_SETTINGS 1u
#define FRAME_LAUNCHER_ORDER_LOGS     2u

#ifdef __cplusplus
}
#endif
