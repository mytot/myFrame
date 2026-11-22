/*
 * frame_core/app.h — the app lifecycle contract.
 *
 * This is the shape every app has, whether it is built into the image, a
 * native image installed into a slot, or a WASM module. The full SDK surface
 * lives in frame_app; this header defines only the lifecycle and context types
 * so that the manager does not have to depend on LVGL or the network stack.
 */
#pragma once

#include "esp_err.h"

#include "frame_core/event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any breaking change to a struct or function signature in the app
 * API. A mismatching image is refused before it executes a single instruction. */
#define FRAME_ABI_VERSION 1u

/*
 * SDK version advertised to the store. A manifest's `sdk` field is a semver
 * range checked against this string; it is bumped with FRAME_ABI_VERSION and
 * whenever the host API grows in a way apps may depend on.
 */
#define FRAME_SDK_VERSION "1.0.0"

/*
 * Opaque service handles. Declared as void* so this header stays free of
 * dependencies; frame_app casts them to the real types.
 */
typedef struct frame_ctx {
    const char *app_id;
    void       *kv;      /* frame_kv_t      */
    void       *fs;      /* frame_fs_t      */
    void       *log;     /* frame_log_t     */
    void       *ui;      /* frame_ui_t      */
    void       *net;     /* frame_net_t     — NULL without "net"      */
    void       *audio;   /* frame_audio_t   — NULL without "audio"    */
    void       *state;   /* app-private, survives resume/pause        */
} frame_ctx_t;

typedef enum {
    FRAME_APP_LIFECYCLE_CREATED = 0,
    FRAME_APP_LIFECYCLE_RUNNING,
    FRAME_APP_LIFECYCLE_PAUSED,
    FRAME_APP_LIFECYCLE_STOPPED,
    FRAME_APP_LIFECYCLE_DESTROYED,
} frame_app_lifecycle_t;

/* Deprecated aliases — remove after December 2026. */
#define FRAME_APP_STATE_CREATED   FRAME_APP_LIFECYCLE_CREATED
#define FRAME_APP_STATE_RUNNING   FRAME_APP_LIFECYCLE_RUNNING
#define FRAME_APP_STATE_PAUSED    FRAME_APP_LIFECYCLE_PAUSED
#define FRAME_APP_STATE_STOPPED   FRAME_APP_LIFECYCLE_STOPPED
#define FRAME_APP_STATE_DESTROYED FRAME_APP_LIFECYCLE_DESTROYED
typedef struct frame_app_descriptor {
    uint32_t    abi_version;         /* must equal FRAME_ABI_VERSION */
    const char *id;
    const char *name;
    const char *version;
    const char *const *permissions;  /* NULL-terminated */
    const char *icon_path;           /* RGB565 asset, optional */

    esp_err_t (*on_create)(frame_ctx_t *ctx);
    esp_err_t (*on_start)(frame_ctx_t *ctx);
    esp_err_t (*on_resume)(frame_ctx_t *ctx);
    esp_err_t (*on_pause)(frame_ctx_t *ctx);
    esp_err_t (*on_stop)(frame_ctx_t *ctx);
    void      (*on_destroy)(frame_ctx_t *ctx);
    void      (*on_event)(frame_ctx_t *ctx, const frame_event_t *evt);
} frame_app_descriptor_t;

/*
 * Health reporting. A freshly booted native image runs in PENDING_VERIFY; the
 * bootloader rolls back unless the app calls this within the health window.
 */
void frame_app_mark_healthy(frame_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
