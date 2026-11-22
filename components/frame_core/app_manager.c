#include "frame_core/app_manager.h"

#include "frame_pure/semver.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "appmgr";

static frame_app_state_t s_apps[FRAME_APP_MAX];
static size_t            s_count;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

/* Built-ins get a stable, documented order so the launcher does not shuffle
 * when the registry changes underneath it. */
static int builtin_order(const char *id)
{
    if (strstr(id, "store") != NULL)    return (int)FRAME_LAUNCHER_ORDER_STORE;
    if (strstr(id, "settings") != NULL) return (int)FRAME_LAUNCHER_ORDER_SETTINGS;
    if (strstr(id, "logs") != NULL)     return (int)FRAME_LAUNCHER_ORDER_LOGS;
    return 99;
}

static int rank(const frame_app_state_t *a)
{
    return a->builtin ? builtin_order(a->id) : 100;
}

static int cmp_state(const void *pa, const void *pb)
{
    const frame_app_state_t *a = (const frame_app_state_t *)pa;
    const frame_app_state_t *b = (const frame_app_state_t *)pb;

    int ra = rank(a);
    int rb = rank(b);
    if (ra != rb) {
        return (ra < rb) ? -1 : 1;
    }
    /* Most recently launched first. */
    if (a->last_launched_ms != b->last_launched_ms) {
        return (a->last_launched_ms > b->last_launched_ms) ? -1 : 1;
    }
    return strcmp(a->name, b->name);
}

esp_err_t frame_app_manager_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_count = 0;
    memset(s_apps, 0, sizeof(s_apps));
    ESP_LOGI(TAG, "app manager initialised (%u slots)", (unsigned)FRAME_APP_MAX);
    return ESP_OK;
}

size_t frame_app_manager_count(void)
{
    lock();
    size_t n = s_count;
    unlock();
    return n;
}

const frame_app_state_t *frame_app_manager_at(size_t index)
{
    if (index >= FRAME_APP_MAX) {
        return NULL;
    }
    return &s_apps[index];
}

const frame_app_state_t *frame_app_manager_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    lock();
    const frame_app_state_t *found = NULL;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            found = &s_apps[i];
            break;
        }
    }
    unlock();
    return found;
}

static void copy_str_field(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

esp_err_t frame_app_manager_upsert(const frame_app_state_t *state)
{
    if (state == NULL || state->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    lock();

    size_t idx = FRAME_APP_MAX;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, state->id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == FRAME_APP_MAX) {
        if (s_count >= FRAME_APP_MAX) {
            unlock();
            return ESP_ERR_NO_MEM;
        }
        idx = s_count++;
        memset(&s_apps[idx], 0, sizeof(s_apps[idx]));
    }

    frame_app_state_t *dst = &s_apps[idx];
    copy_str_field(dst->id, sizeof(dst->id), state->id);
    copy_str_field(dst->name, sizeof(dst->name), state->name);
    copy_str_field(dst->version, sizeof(dst->version), state->version);
    copy_str_field(dst->icon_path, sizeof(dst->icon_path), state->icon_path);
    dst->kind       = state->kind;
    dst->slot       = state->slot;
    dst->builtin    = state->builtin;
    dst->healthy    = state->healthy;

    /* Preserve the cached "newest available" version across re-upserts so a
     * registry refresh does not clear the launcher badges. */
    if (state->available_version[0] != '\0') {
        copy_str_field(dst->available_version, sizeof(dst->available_version),
                       state->available_version);
    }

    frame_semver_t have, want;
    dst->update_available = false;
    if (frame_semver_parse(dst->version, &have) == 0 &&
        dst->available_version[0] != '\0' &&
        frame_semver_parse(dst->available_version, &want) == 0 &&
        frame_semver_cmp(&want, &have) > 0) {
        dst->update_available = true;
    }

    unlock();
    frame_app_manager_sort();
    return ESP_OK;
}

esp_err_t frame_app_manager_remove(const char *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            if (s_apps[i].builtin) {
                unlock();
                return ESP_ERR_INVALID_STATE;   /* built-ins cannot be removed */
            }
            memmove(&s_apps[i], &s_apps[i + 1u], (s_count - i - 1u) * sizeof(s_apps[0]));
            s_count--;
            memset(&s_apps[s_count], 0, sizeof(s_apps[0]));
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t frame_app_manager_set_available(const char *id, const char *version)
{
    if (id == NULL || version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t rc = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) != 0) {
            continue;
        }
        copy_str_field(s_apps[i].available_version, sizeof(s_apps[i].available_version), version);

        frame_semver_t have, want;
        s_apps[i].update_available = false;
        if (frame_semver_parse(s_apps[i].version, &have) == 0 &&
            frame_semver_parse(version, &want) == 0 &&
            frame_semver_cmp(&want, &have) > 0) {
            s_apps[i].update_available = true;
        }
        rc = ESP_OK;
        break;
    }
    unlock();
    return rc;
}

void frame_app_manager_clear_available(const char *id)
{
    if (id == NULL) {
        return;
    }
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            s_apps[i].available_version[0] = '\0';
            s_apps[i].update_available = false;
            break;
        }
    }
    unlock();
}

uint32_t frame_app_manager_update_count(void)
{
    lock();
    uint32_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_apps[i].update_available) {
            n++;
        }
    }
    unlock();
    return n;
}

void frame_app_manager_mark_launched(const char *id, uint32_t now_ms)
{
    if (id == NULL) {
        return;
    }
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            s_apps[i].last_launched_ms = now_ms;
            break;
        }
    }
    unlock();
    frame_app_manager_sort();
}

void frame_app_manager_sort(void)
{
    /* Insertion sort: the list is small and this is stable, so equal-ranked
     * apps keep their relative order between calls. */
    lock();
    for (size_t i = 1; i < s_count; i++) {
        frame_app_state_t key = s_apps[i];
        size_t j = i;
        while (j > 0 && cmp_state(&s_apps[j - 1u], &key) > 0) {
            s_apps[j] = s_apps[j - 1u];
            j--;
        }
        s_apps[j] = key;
    }
    unlock();
}

void frame_app_manager_mark_unhealthy(const char *id)
{
    if (id == NULL) {
        return;
    }
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            s_apps[i].healthy = false;
            break;
        }
    }
    unlock();
}

void frame_app_manager_mark_healthy(const char *id)
{
    if (id == NULL) {
        return;
    }
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            s_apps[i].healthy = true;
            break;
        }
    }
    unlock();
}

/* ------------------------------------------------------------------ */
/* Health reporting                                                     */
/* ------------------------------------------------------------------ */

/*
 * Called by an app once it is confident it is working. In practice this clears
 * the bootloader's PENDING_VERIFY state for a freshly installed native image,
 * so a crash before this point rolls back to the previous slot.
 *
 * Implemented here rather than in frame_ota so that built-in apps can call it
 * too without linking the OTA stack.
 */
void frame_app_mark_healthy(frame_ctx_t *ctx)
{
    if (ctx == NULL || ctx->app_id == NULL) {
        return;
    }
    frame_app_manager_mark_healthy(ctx->app_id);
}
