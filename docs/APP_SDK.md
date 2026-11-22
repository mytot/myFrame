# myFrame — App SDK (`frame_app`)

---

## 1. What an app is

An app is a **unit of independently installable functionality** with a stable identity (`id`),
a version, and a declared set of requirements and permissions. It is built against `frame_app`
and — for native apps — linked into a standalone firmware image that carries the SDK.

Apps never talk to ESP-IDF, LVGL or the board directly. They see exactly one header:

```c
#include <frame/frame.h>
```

---

## 2. App descriptor

Every app exports one symbol, `frame_app_descriptor`. The shell reads it; the linker enforces it.

```c
typedef struct frame_app_descriptor {
    uint32_t abi_version;             // must equal FRAME_ABI_VERSION
    const char *id;                   // "dev.frame.clock"
    const char *name;                 // "Clock"
    const char *version;              // "1.2.0"
    const uint8_t *icon_rgb565;       // 64×64 RGB565, optional
    uint32_t icon_len;

    const char *const *permissions;   // NULL-terminated

    // Lifecycle ---------------------------------------------------------
    esp_err_t (*on_create)(frame_ctx_t *ctx);
    esp_err_t (*on_start)(frame_ctx_t *ctx);
    esp_err_t (*on_resume)(frame_ctx_t *ctx);
    esp_err_t (*on_pause)(frame_ctx_t *ctx);
    esp_err_t (*on_stop)(frame_ctx_t *ctx);
    void      (*on_destroy)(frame_ctx_t *ctx);

    // Events ------------------------------------------------------------
    void (*on_event)(frame_ctx_t *ctx, const frame_event_t *ev);
} frame_app_descriptor_t;

#define FRAME_APP_EXPORT(desc) \
    __attribute__((section(".frame_app"))) \
    const frame_app_descriptor_t *const frame_app_entry = &(desc);
```

`FRAME_ABI_VERSION` is a `uint32_t` bumped on **every** breaking change to any struct in
`frame_app`. An image whose `abi_version` does not match the running FrameOS is refused at boot —
checked in the app's first instructions, before any SDK call.

---

## 3. Context

```c
typedef struct frame_ctx {
    const char *app_id;
    frame_kv_t      *kv;        // app-private key/value store (NVS namespace)
    frame_fs_t      *fs;        // app-private directory /apps/<id>/
    frame_log_t     *log;       // tagged logger
    frame_ui_t      *ui;        // the app's LVGL screen (already created)
    frame_net_t     *net;       // NULL unless "net" permission granted
    frame_audio_t   *audio;     // NULL unless "audio" permission granted
    void            *state;     // app's own opaque pointer, survives across resumes
} frame_ctx_t;
```

`ctx` is allocated by the shell, is stable for the app's lifetime, and is the *only* handle an app
receives. Everything else is reachable from it.

---

## 4. API surface

Kept deliberately small — roughly 60 functions total, all synchronous except where noted.

### 4.1 UI

```c
/* The app owns exactly one screen. It is created empty, full-size, and pushed on start. */
lv_obj_t *frame_ui_root(frame_ctx_t *ctx);

/* Themed widgets — use these, not raw lv_obj_create, so apps inherit the device theme. */
lv_obj_t *frame_ui_label (frame_ctx_t *ctx, lv_obj_t *parent, const char *text);
lv_obj_t *frame_ui_button(frame_ctx_t *ctx, lv_obj_t *parent, const char *text,
                          frame_ui_cb_t cb, void *user);
lv_obj_t *frame_ui_list  (frame_ctx_t *ctx, lv_obj_t *parent);

/* Virtualised list: materialises only visible rows. A 10 000-row list must not
 * cost 10 000 LVGL objects — this is the main way an app stays inside its budget. */
lv_obj_t *frame_ui_virtual_list(frame_ctx_t *ctx, lv_obj_t *parent,
                                uint32_t count, uint32_t row_h,
                                frame_ui_row_cb_t bind, void *user);

/* Navigation */
esp_err_t frame_ui_toast  (frame_ctx_t *ctx, const char *msg, uint32_t ms);
esp_err_t frame_ui_confirm(frame_ctx_t *ctx, const char *title, const char *body,
                           frame_ui_confirm_cb_t cb, void *user);

/* Themed read-only for apps — the shell owns it. */
const frame_theme_t *frame_ui_theme(frame_ctx_t *ctx);
```

### 4.1a Render budget — what an app is allowed to cost

PSRAM bandwidth is the scarce resource on this hardware (see
[Render Performance](RENDER_PERFORMANCE.md)), so rendering is a **declared contract**, not a hope.

```c
typedef enum {
    FRAME_RENDER_CLASS_STATIC,   /* on demand, only on change      */
    FRAME_RENDER_CLASS_LIGHT,    /* 30 Hz, ≤ 15 % of screen        */
    FRAME_RENDER_CLASS_MEDIUM,   /* 30 Hz, ≤ 40 % of screen        */
    FRAME_RENDER_CLASS_FULL,     /* 20 Hz, transitions only        */
} frame_render_class_t;

/* Read-only: the class declared in this app's manifest. */
frame_render_class_t frame_ui_render_class(frame_ctx_t *ctx);

/* Clamp the app's own animation timer. Values above the class cap are clamped. */
esp_err_t frame_ui_set_fps_cap(frame_ctx_t *ctx, uint8_t fps);

/* Cache a complex static region: render once, blit thereafter. */
lv_obj_t *frame_ui_surface_cache(frame_ctx_t *ctx, lv_obj_t *parent,
                                 uint16_t w, uint16_t h, frame_ui_paint_cb_t paint);

/* Self-diagnostic, mirrored in the debug overlay. */
void frame_ui_budget_report(frame_ctx_t *ctx, frame_render_stats_t *out);
```

**Enforced by the shell:**

| Mechanism | Behaviour |
|-----------|-----------|
| FPS cap | Clamped to the declared class regardless of what the app requests |
| Invalidate accounting | `dirty_px` attributed to the running app, shown in the debug overlay |
| Budget warning | > 5 consecutive frames over budget → logged warning + overlay flag (not a hard failure) |
| `direct` opt-in | Honoured only if the manifest declares it **and** the board profile allows it; otherwise silently downgraded to `balanced` |

**The two rules that matter most:**

1. **Invalidate only what changed.** `lv_obj_invalidate(lv_screen_active())` costs ~38× more PSRAM
   traffic than invalidating the widget that actually changed.
2. **Never create a full-screen animation at 30 Hz.** A full-screen 800×480 update is ~24 ms of
   render+copy; two of those do not fit in a 33 ms frame. Use LVGL's built-in transitions, which
   invalidate only the animating region.

Full budget arithmetic and the per-class cost table: [Render Performance §1.4 and §7](RENDER_PERFORMANCE.md).

Apps get a themed, full-screen surface and themed widget factories. They do **not** get to change
the status bar, register global gestures, or draw outside their screen.

### 4.2 Storage

```c
esp_err_t frame_kv_set_str (frame_kv_t *kv, const char *key, const char *val);
esp_err_t frame_kv_get_str (frame_kv_t *kv, const char *key, char *out, size_t len);
esp_err_t frame_kv_set_u32 (frame_kv_t *kv, const char *key, uint32_t val);
esp_err_t frame_kv_get_u32 (frame_kv_t *kv, const char *key, uint32_t *out);
esp_err_t frame_kv_set_blob(frame_kv_t *kv, const char *key, const void *buf, size_t len);
esp_err_t frame_kv_get_blob(frame_kv_t *kv, const char *key, void *buf, size_t *len);
esp_err_t frame_kv_del     (frame_kv_t *kv, const char *key);

/* Files, scoped to /apps/<id>/ — traversal outside is refused. */
esp_err_t frame_fs_write (frame_fs_t *fs, const char *path, const void *buf, size_t len);
esp_err_t frame_fs_read  (frame_fs_t *fs, const char *path, void *buf, size_t len, size_t *out);
esp_err_t frame_fs_remove(frame_fs_t *fs, const char *path);
```

### 4.3 Networking (requires `net`)

```c
typedef void (*frame_http_cb_t)(void *user, int status, const uint8_t *body, size_t len);

esp_err_t frame_http_get(frame_net_t *net, const char *url,
                         frame_http_cb_t cb, void *user);   /* async, callback on net task */
esp_err_t frame_http_post(frame_net_t *net, const char *url,
                          const char *ctype, const uint8_t *body, size_t len,
                          frame_http_cb_t cb, void *user);
esp_err_t frame_net_time_sync(frame_net_t *net);   /* NTP, idempotent */
```

Callbacks fire on the **net task**. An app that touches LVGL from a callback must use
`frame_ui_dispatch()`:

```c
void frame_ui_dispatch(frame_ctx_t *ctx, void (*fn)(void *), void *user);
```

### 4.4 System

```c
/* Report health. Called by the app runtime automatically after FRAME_HEALTH_TIMEOUT_MS
 * of successful operation. Apps may call it early; they must not call it if unhealthy. */
void frame_app_mark_healthy(frame_ctx_t *ctx);

/* Ask to leave. Returns to the launcher. */
void frame_exit_to_shell(frame_ctx_t *ctx);

/* Device information — no identifiers, no secrets. */
typedef struct {
    const char *device_id;      /* "sunton_esp32_8048s070"  */
    const char *chip;           /* "esp32s3"                */
    uint32_t    psram_size;
    uint16_t    screen_w, screen_h;
    uint8_t     battery_pct;    /* 255 = no battery          */
    int8_t      wifi_rssi;      /* 127 = not connected       */
} frame_device_info_t;

void frame_get_device_info(frame_ctx_t *ctx, frame_device_info_t *out);
void frame_log(frame_ctx_t *ctx, frame_log_level_t lvl, const char *tag, const char *fmt, ...);
```

---

## 5. Lifecycle

```
            ┌──────────┐
            │  CREATED │  on_create(): allocate, build UI, bind events
            └────┬─────┘
                 │ on_start()
            ┌────▼─────┐
     ┌─────►│ RUNNING  │◄──────────────┐
     │      └────┬─────┘               │
     │on_resume()│                     │on_resume()
     │      ┌────▼─────┐          ┌────┴─────┐
     └──────│ PAUSED   │──────────│ STOPPED  │
            └──────────┘ on_stop()└──────────┘
                                   │ on_destroy()
                                   ▼
                                 freed
```

| State | Meaning | App must |
|-------|---------|----------|
| `CREATED` | `on_create()` ran | Not touch hardware yet; build UI only |
| `RUNNING` | foreground, visible | Render, respond to events |
| `PAUSED` | another app / dialog is on top | Stop animations, drop timers, release audio |
| `STOPPED` | not visible | Save state to `kv` immediately |
| destroyed | `on_destroy()` ran | Release everything |

**The one rule that matters:** `on_pause()` may be the last call the app ever gets. Persist anything
the user would miss in `on_pause()`, not `on_destroy()`.

---

## 6. Permissions

| Permission | Grants | Enforced |
|------------|--------|----------|
| `net` | `frame_http_*`, `frame_net_time_sync` | **Yes** — `ctx->net` is `NULL` without it; native apps that link the SDK still get `NULL` and fail cleanly |
| `storage:app` | `frame_fs_*` within `/apps/<id>/` | **Yes** — path traversal rejected in the service layer |
| `audio` | `frame_audio_*` | **Yes** — `ctx->audio` is `NULL` |
| `location` | reserved | n/a in v1 |

Stated plainly: **native apps run with full CPU privileges** because they are separate firmware
images — a native app *could* bypass the SDK. Permissions are therefore (a) an honest disclosure to
the user at install time, (b) a hard boundary for managed apps, and (c) a soft boundary that keeps
well-behaved native apps portable. This is documented rather than hidden; pretending otherwise
would be worse.

---

## 7. Sample native app

```c
#include <frame/frame.h>

static frame_ctx_t *s_ctx;
static lv_obj_t *s_time_label;

static void tick_cb(void *user) {
    frame_time_t now;
    frame_time_now(&now);
    char buf[16];
    snprintf(buf, sizeof buf, "%02u:%02u", now.hour, now.min);
    lv_label_set_text(s_time_label, buf);
}

static esp_err_t on_create(frame_ctx_t *ctx) {
    s_ctx = ctx;
    lv_obj_t *root = frame_ui_root(ctx);

    s_time_label = frame_ui_label(ctx, root, "--:--");
    lv_obj_set_style_text_font(s_time_label, frame_ui_theme(ctx)->font_display, 0);
    lv_obj_center(s_time_label);

    frame_time_subscribe(ctx, tick_cb, NULL);   /* fires once a minute */
    return ESP_OK;
}

static void on_destroy(frame_ctx_t *ctx) {
    frame_time_unsubscribe(ctx, tick_cb, NULL);
}

FRAME_APP_EXPORT((frame_app_descriptor_t) {
    .abi_version = FRAME_ABI_VERSION,
    .id          = "dev.frame.clock",
    .name        = "Clock",
    .version     = "1.2.0",
    .permissions = (const char *[]){ NULL },
    .on_create   = on_create,
    .on_destroy  = on_destroy,
});
```

---

## 8. Managed apps (WASM) — shipped in v1

Managed apps are `.wasm` modules targeting a small host API. They get **no LVGL pointers** —
instead they emit a declarative UI tree that the shell diffs and applies. This keeps the ABI tiny,
stable, and safe across the interpreter boundary.

Same package format, same catalog, same signing, same store UI as native apps. `manifest.kind`
is `managed` and `payload.bin` is a `.wasm` module instead of an ESP image.

```c
/* wasi-ish subset + frame host imports, callable from any language */
void  frame_ui_begin(void);
int   frame_ui_node(int parent, uint8_t type, const char *id, const uint8_t *props, int len);
void  frame_ui_set_text(int node, const char *utf8);
void  frame_ui_set_style(int node, const char *prop, const char *value);
void  frame_ui_end(void);          /* shell diffs tree → LVGL */

void  frame_log_i(const char *tag, const char *msg);
int   frame_http_get(const char *url, int *handle);   /* async, polled */
int   frame_kv_get(const char *key, char *out, int cap);
```

```rust
// The same clock, in Rust
#[no_mangle]
pub extern "C" fn frame_app_main() {
    unsafe {
        frame_ui_begin();
        let root = frame_ui_node(0, NODE_CONTAINER, c"root".as_ptr(), ptr::null(), 0);
        let lbl  = frame_ui_node(root, NODE_LABEL, c"time".as_ptr(), ptr::null(), 0);
        frame_ui_set_style(lbl, c"text_font".as_ptr(), c"display".as_ptr());
        frame_ui_set_text(lbl, c"12:34".as_ptr());
        frame_ui_end();
    }
}
```

Trade-offs, restated so nobody is surprised later:

| | |
|---|---|
| ✅ | Hot install / switch with **no reboot** (~5–20 ms) |
| ✅ | Genuine sandbox — a trap cannot corrupt the shell |
| ✅ | Same package format, catalog and signing as native |
| ✅ | Write in C, Rust, TinyGo, AssemblyScript |
| ❌ | ~10–40× slower than native |
| ❌ | Bounded heap (64–512 KB) |
| ❌ | UI must fit the retained-tree model — no custom drawing |
| ❌ | Async/networking needs explicit polling discipline |

**Performance gate (Phase 7).** Because WASM is in v1 rather than deferred, it gets a hard
acceptance gate: a 200-row scrolling list must sustain **≥ 20 fps** in the managed runtime before
WASM is used for anything beyond simple dashboards. If the gate fails, native remains the primary
path and WASM is documented as a widget-class runtime — but the package format does not change.

---

## 9. ABI stability

| Rule | Detail |
|------|--------|
| `FRAME_ABI_VERSION` | Bumped on **any** change to a struct layout, enum value, or function signature in `frame_app` |
| Additive changes | New functions at the **end** of the export table — never reorder |
| Struct growth | New fields only at the end, with an explicit `_reserved` tail |
| Compile-time check | `FRAME_STATIC_ASSERT(FRAME_ABI_VERSION == 1)` in every app; mismatch is a link error |
| Runtime check | Boot rejects an image whose `abi_version` ≠ FrameOS's, before the app runs |
| Deprecation | A function is deprecated for ≥ 2 minor releases before removal |

The export table is emitted as a versioned symbol block in the linker script, so the check is a
single `memcmp` at boot.

---

## 10. Host-side testing

Pure-logic components are host-testable because they never touch hardware (architecture rule **L3**):

| Component | Host test |
|-----------|-----------|
| `frame_ota` slot allocator | eviction, reservation expiry, sweep-after-power-loss |
| `frame_store` package verifier | valid/invalid signature, tampered payload, wrong key |
| `frame_app` semver + requirement matching | `^1.0.0` ranges, `requires` negotiation |
| `frame_storage` registry | atomic swap, orphan cleanup |

```bash
idf.py build                                                  # target build
cmake -B build/host -DFRAME_HOST_TESTS=ON && cmake --build build/host && ctest
```

The same verifier, the same semver code and the same slot allocator run on the laptop and on the
device. That is the point of the layering rules in [Architecture](ARCHITECTURE.md#2-layering-rules).
