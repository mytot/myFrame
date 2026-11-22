/*
 * wasm.c — managed app runtime on wasm3.
 *
 * Threading rule: the wasm3 runtime is only ever touched from the wasm task
 * (core 1, priority 3, below LVGL). Launches and stops are requested through a
 * queue; the task calls the module's exported frame_app_event() /
 * frame_app_tick() and the module's host imports record UI operations that the
 * LVGL task applies (ui_tree.c).
 *
 * Nothing in a module can block the UI thread, touch LVGL, or outlive its own
 * runtime: a trap ends the app, not the device.
 *
 * Launch protocol: frame_wasm_launch() posts MSG_LAUNCH and waits for the task
 * to set s_launch_result (+1 started, -1 failed). The task owns the runtime;
 * the caller only ever reads two volatile flags, so there is no shared state
 * to corrupt.
 */
#include "frame_wasm/wasm.h"

#include "frame_core/app.h"
#include "frame_core/app_manager.h"
#include "frame_core/event.h"
#include "frame_log/log.h"
#include "frame_pure/manifest.h"
#include "frame_storage/storage.h"
#include "frame_store/store.h"
#include "frame_ui/ui.h"
#include "ui_tree.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "wasm3.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wasm";

#ifndef CONFIG_FRAME_WASM_STACK_KB
#define CONFIG_FRAME_WASM_STACK_KB 64
#endif
#ifndef CONFIG_FRAME_WASM_HEAP_MAX_KB
#define CONFIG_FRAME_WASM_HEAP_MAX_KB 512
#endif
#ifndef CONFIG_FRAME_WASM_TASK_STACK
#define CONFIG_FRAME_WASM_TASK_STACK 6144
#endif

#define MODULE_MAX_BYTES      (4u * 1024u * 1024u)
#define LAUNCH_TIMEOUT_MS     4000u
#define STOP_TIMEOUT_MS       2000u
#define TEXT_MAX              48u

/* ------------------------------------------------------------------ */
/* Message queue                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    MSG_LAUNCH = 1,   /* app_id = module to start */
    MSG_STOP,         /* stop the running module                 */
    MSG_EVENT,        /* type/a/b -> frame_app_event()           */
} msg_kind_t;

typedef struct {
    int32_t kind;
    int32_t type;
    int32_t a;
    int32_t b;
    char    app_id[FRAME_APP_ID_MAX];
} wasm_msg_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;
static bool          s_inited;

/* Written by the wasm task, read by everyone. */
static volatile bool s_running;
static volatile int  s_launch_result;    /* 0 pending, 1 ok, -1 failed */
static char          s_app_id[FRAME_APP_ID_MAX];

/* Owned by the wasm task. */
static IM3Environment s_env;
static IM3Runtime     s_runtime;
static IM3Module      s_module;
static bool           s_module_loaded;   /* ownership passed to the runtime */
static uint8_t       *s_bytes;
static size_t         s_bytes_len;
static IM3Function    s_fn_main;
static IM3Function    s_fn_event;
static IM3Function    s_fn_tick;
static char           s_app_name[FRAME_APP_NAME_MAX];
static bool           s_stop_requested;  /* task-private */
static size_t         s_linear_bytes;

/* ------------------------------------------------------------------ */
/* Helpers over wasm memory                                            */
/* ------------------------------------------------------------------ */

/* Copies a NUL-terminated string out of the module's linear memory into a
 * bounded local buffer. Returns NULL on an out-of-bounds or unterminated
 * string, so a module can never make the host read past its memory. */
static const char *mem_str(void *_mem, IM3Runtime runtime, uint32_t ptr, char *buf, size_t cap)
{
    const size_t mem_size = m3_GetMemorySize(runtime);
    if (ptr == 0u || ptr >= mem_size || cap == 0u) {
        return NULL;
    }
    const char *src = (const char *)_mem + ptr;
    const size_t avail = mem_size - ptr;
    const size_t n = strnlen(src, avail);
    if (n == avail) {
        return NULL;    /* no terminator inside linear memory */
    }
    const size_t copy = (n < cap) ? n : cap - 1u;
    memcpy(buf, src, copy);
    buf[copy] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Host imports — the ABI in docs/APP_SDK.md §8                         */
/* ------------------------------------------------------------------ */

m3ApiRawFunction(m3_frame_ui_begin)
{
    (void)runtime; (void)_ctx; (void)_sp; (void)_mem;
    ui_tree_reset();
    m3ApiSuccess();
}

/* "k=v;k=v" -> one set_style op per pair. Bounded so a hostile props string
 * cannot flood the op buffer on its own (the recorder clips anyway). */
static void apply_props(int32_t handle, const char *props, uint32_t len)
{
    char buf[128];
    if (props == NULL || len == 0u) {
        return;
    }
    const size_t n = (len < sizeof(buf) - 1u) ? len : sizeof(buf) - 1u;
    memcpy(buf, props, n);
    buf[n] = '\0';

    char *save = NULL;
    unsigned pairs = 0;
    for (char *kv = strtok_r(buf, ";", &save); kv != NULL && pairs < 8u;
         kv = strtok_r(NULL, ";", &save), pairs++) {
        char *eq = strchr(kv, '=');
        if (eq == NULL || eq == kv) {
            continue;
        }
        *eq = '\0';
        ui_tree_record_style(handle, kv, eq + 1);
    }
}

/*
 * frame_ui_node(parent, type, id, props, len) -> handle
 *
 * The host assigns handles: a module cannot forge a pointer to a shell
 * object, and a stale handle simply matches no node. Handles restart at 1
 * for every session because the node table is per session.
 */
m3ApiRawFunction(m3_frame_ui_node)
{
    (void)_ctx; (void)_sp;
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(uint32_t, parent)
    m3ApiGetArg(uint32_t, type)
    m3ApiGetArg(uint32_t, id_ptr)
    m3ApiGetArg(uint32_t, props_ptr)
    m3ApiGetArg(uint32_t, props_len)

    static int32_t s_next_handle = 1;
    const int32_t handle = s_next_handle++;
    if (s_next_handle > 100000) {
        s_next_handle = 1;
    }

    char id[TEXT_MAX];
    const char *ids = mem_str(_mem, runtime, id_ptr, id, sizeof(id));
    ui_tree_record_node(handle, (int32_t)parent, (int32_t)type, ids);

    if (props_ptr != 0u && props_len > 0u) {
        const size_t mem_size = m3_GetMemorySize(runtime);
        if ((uint64_t)props_ptr + props_len <= (uint64_t)mem_size) {
            char props[160];
            const size_t n = (props_len < sizeof(props)) ? props_len : sizeof(props) - 1u;
            memcpy(props, (const uint8_t *)_mem + props_ptr, n);
            props[n] = '\0';
            apply_props(handle, props, (uint32_t)n);
        }
    }
    m3ApiReturn((uint32_t)handle);
}

m3ApiRawFunction(m3_frame_ui_set_text)
{
    (void)_ctx; (void)_sp;
    m3ApiGetArg(uint32_t, node)
    m3ApiGetArg(uint32_t, text_ptr)
    char text[TEXT_MAX];
    const char *t = mem_str(_mem, runtime, text_ptr, text, sizeof(text));
    if (t != NULL) {
        ui_tree_record_text((int32_t)node, t);
    }
    m3ApiSuccess();
}

m3ApiRawFunction(m3_frame_ui_set_style)
{
    (void)_ctx; (void)_sp;
    m3ApiGetArg(uint32_t, node)
    m3ApiGetArg(uint32_t, prop_ptr)
    m3ApiGetArg(uint32_t, value_ptr)
    char prop[40];
    char value[40];
    const char *p = mem_str(_mem, runtime, prop_ptr, prop, sizeof(prop));
    const char *v = mem_str(_mem, runtime, value_ptr, value, sizeof(value));
    if (p != NULL && v != NULL) {
        ui_tree_record_style((int32_t)node, p, v);
    }
    m3ApiSuccess();
}

m3ApiRawFunction(m3_frame_ui_end)
{
    (void)runtime; (void)_ctx; (void)_sp; (void)_mem;
    ui_tree_commit();
    m3ApiSuccess();
}

/* frame_ui_set_title is dispatch-safe, so this is legal from the wasm task. */
m3ApiRawFunction(m3_frame_ui_title)
{
    (void)_ctx; (void)_sp;
    m3ApiGetArg(uint32_t, title_ptr)
    char title[FRAME_UI_MAX_TITLE];
    const char *t = mem_str(_mem, runtime, title_ptr, title, sizeof(title));
    if (t != NULL) {
        frame_ui_set_title(t);
    }
    m3ApiSuccess();
}

static void log_from_module(frame_log_level_t level, void *_mem, IM3Runtime runtime,
                            uint32_t tag_ptr, uint32_t msg_ptr)
{
    char tag[FRAME_LOG_TAG_MAX];
    char msg[96];
    const char *tg = mem_str(_mem, runtime, tag_ptr, tag, sizeof(tag));
    const char *m  = mem_str(_mem, runtime, msg_ptr, msg, sizeof(msg));
    if (m != NULL) {
        frame_log(level, tg ? tg : s_app_id, "%s", m);
    }
}

m3ApiRawFunction(m3_frame_log_i)
{
    (void)_ctx; (void)_sp;
    m3ApiGetArg(uint32_t, tag_ptr)
    m3ApiGetArg(uint32_t, msg_ptr)
    log_from_module(FRAME_LOG_INFO, _mem, runtime, tag_ptr, msg_ptr);
    m3ApiSuccess();
}

m3ApiRawFunction(m3_frame_log_w)
{
    (void)_ctx; (void)_sp;
    m3ApiGetArg(uint32_t, tag_ptr)
    m3ApiGetArg(uint32_t, msg_ptr)
    log_from_module(FRAME_LOG_WARN, _mem, runtime, tag_ptr, msg_ptr);
    m3ApiSuccess();
}

m3ApiRawFunction(m3_frame_now_ms)
{
    (void)_ctx; (void)_mem; (void)runtime; (void)_sp;
    m3ApiReturnType(uint32_t)
    m3ApiReturn((uint32_t)(esp_timer_get_time() / 1000));
}

/* Key/value scoped to the app: keys are namespaced with the app id so two
 * modules cannot read each other's settings. */
m3ApiRawFunction(m3_frame_kv_get)
{
    (void)_ctx; (void)_sp;
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(uint32_t, key_ptr)
    m3ApiGetArg(uint32_t, out_ptr)
    m3ApiGetArg(uint32_t, cap)

    char key[64];
    const char *k = mem_str(_mem, runtime, key_ptr, key, sizeof(key));
    if (k == NULL || out_ptr == 0u || cap == 0u) {
        m3ApiReturn(0);
    }
    char full[FRAME_APP_ID_MAX + 64];
    snprintf(full, sizeof(full), "app.%s.%s", s_app_id, k);

    char val[128];
    if (cap > sizeof(val)) {
        cap = sizeof(val);
    }
    if (frame_settings_get_str(full, val, cap, NULL) != ESP_OK) {
        m3ApiReturn(0);
    }
    const size_t n = strlen(val);
    if ((uint64_t)out_ptr + n + 1u > (uint64_t)m3_GetMemorySize(runtime)) {
        m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    }
    memcpy((uint8_t *)_mem + out_ptr, val, n + 1u);
    m3ApiReturn((uint32_t)n);
}

m3ApiRawFunction(m3_frame_kv_set)
{
    (void)_ctx; (void)_sp;
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(uint32_t, key_ptr)
    m3ApiGetArg(uint32_t, val_ptr)
    char key[64];
    char val[128];
    const char *k = mem_str(_mem, runtime, key_ptr, key, sizeof(key));
    const char *v = mem_str(_mem, runtime, val_ptr, val, sizeof(val));
    if (k == NULL || v == NULL) {
        m3ApiReturn(0);
    }
    char full[FRAME_APP_ID_MAX + 64];
    snprintf(full, sizeof(full), "app.%s.%s", s_app_id, k);
    m3ApiReturn(frame_settings_set_str(full, v) == ESP_OK ? 1u : 0u);
}

/* Networking is declared in the SDK but deliberately not linked in v1: the
 * module gets a clean \"unsupported\" rather than a link error, so the same
 * .wasm runs once the async HTTP host function ships. */
m3ApiRawFunction(m3_frame_http_get)
{
    (void)_ctx; (void)_mem; (void)runtime; (void)_sp;
    m3ApiReturnType(int32_t)
    m3ApiGetArg(uint32_t, url_ptr)
    m3ApiGetArg(uint32_t, handle_ptr)
    (void)url_ptr; (void)handle_ptr;
    m3ApiReturn(-1);
}

m3ApiRawFunction(m3_frame_app_exit)
{
    (void)runtime; (void)_ctx; (void)_sp; (void)_mem;
    wasm_msg_t m = { .kind = MSG_STOP };
    (void)xQueueSend(s_queue, &m, 0);
    m3ApiSuccess();
}

/* ------------------------------------------------------------------ */
/* Linking                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *sig;
    M3RawCall  fn;
} import_t;

#define IMPORT(n, s, f) { n, s, (M3RawCall)f }

static const import_t k_imports[] = {
    IMPORT("frame_ui_begin",     "v()",       m3_frame_ui_begin),
    IMPORT("frame_ui_node",      "i(iiiii)",  m3_frame_ui_node),
    IMPORT("frame_ui_set_text",  "v(ii)",     m3_frame_ui_set_text),
    IMPORT("frame_ui_set_style", "v(iii)",    m3_frame_ui_set_style),
    IMPORT("frame_ui_end",       "v()",       m3_frame_ui_end),
    IMPORT("frame_ui_title",     "v(i)",      m3_frame_ui_title),
    IMPORT("frame_log_i",        "v(ii)",     m3_frame_log_i),
    IMPORT("frame_log_w",        "v(ii)",     m3_frame_log_w),
    IMPORT("frame_now_ms",       "i()",       m3_frame_now_ms),
    IMPORT("frame_kv_get",       "i(iii)",    m3_frame_kv_get),
    IMPORT("frame_kv_set",       "i(ii)",     m3_frame_kv_set),
    IMPORT("frame_http_get",     "i(ii)",     m3_frame_http_get),
    IMPORT("frame_app_exit",     "v()",       m3_frame_app_exit),
};

static M3Result link_all(void)
{
    for (size_t i = 0; i < sizeof(k_imports) / sizeof(k_imports[0]); i++) {
        M3Result r = m3_LinkRawFunction(s_module, "env", k_imports[i].name, k_imports[i].sig,
                                        k_imports[i].fn);
        if (r == m3Err_functionLookupFailed) {
            continue;   /* the module does not import this one: not an error */
        }
        if (r != m3Err_none) {
            return r;
        }
    }
    return m3Err_none;
}

/* ------------------------------------------------------------------ */
/* Runtime lifecycle (wasm task)                                       */
/* ------------------------------------------------------------------ */

static void runtime_release(void)
{
    s_fn_main  = NULL;
    s_fn_event = NULL;
    s_fn_tick  = NULL;
    if (s_runtime != NULL) {
        m3_ReleaseRuntime(s_runtime);   /* frees the modules it had loaded */
        s_runtime = NULL;
    }
    /* A parsed module whose LoadModule failed is still ours to free. */
    if (!s_module_loaded && s_module != NULL) {
        m3_FreeModule(s_module);
    }
    s_module        = NULL;
    s_module_loaded = false;
    if (s_bytes != NULL) {
        heap_caps_free(s_bytes);
        s_bytes = NULL;
    }
    s_bytes_len    = 0;
    s_linear_bytes = 0;
}

static void report(const char *stage, const char *what)
{
    M3ErrorInfo info = { 0 };
    if (s_runtime != NULL) {
        m3_GetErrorInfo(s_runtime, &info);
        ESP_LOGE(TAG, "%s: %s (%s:%d %s)", stage, what ? what : "?",
                 info.file ? info.file : "?", (int)info.line,
                 info.message ? info.message : "");
    } else {
        ESP_LOGE(TAG, "%s: %s", stage, what ? what : "?");
    }
    frame_ui_toast("%s: %s", s_app_name[0] != '\0' ? s_app_name : "App", what ? what : "error");
    if (s_app_id[0] != '\0') {
        frame_app_manager_mark_unhealthy(s_app_id);
        frame_event_t e = { .type = FRAME_EVT_APP_ERROR };
        snprintf(e.text, sizeof(e.text), "%.31s", what ? what : "wasm error");
        (void)frame_event_post(&e);
    }
}

static bool load_module(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s", path);
        report("open", "module file missing");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)MODULE_MAX_BYTES) {
        fclose(f);
        report("open", "module size invalid");
        return false;
    }
    s_bytes = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_bytes == NULL) {
        s_bytes = malloc((size_t)sz);
    }
    if (s_bytes == NULL) {
        fclose(f);
        report("open", "out of memory");
        return false;
    }
    s_bytes_len = fread(s_bytes, 1, (size_t)sz, f);
    fclose(f);
    if (s_bytes_len != (size_t)sz) {
        report("open", "module read failed");
        return false;
    }

    M3Result r = m3_ParseModule(s_env, &s_module, s_bytes, (uint32_t)s_bytes_len);
    if (r != m3Err_none) {
        report("parse", r);
        return false;
    }
    r = m3_LoadModule(s_runtime, s_module);
    if (r != m3Err_none) {
        report("load", r);
        return false;
    }
    s_module_loaded = true;
    s_linear_bytes = m3_GetMemorySize(s_runtime);
    if (s_linear_bytes > (size_t)CONFIG_FRAME_WASM_HEAP_MAX_KB * 1024u) {
        report("memory", "module wants more linear memory than allowed");
        return false;
    }

    r = link_all();
    if (r != m3Err_none) {
        report("link", r);
        return false;
    }

    r = m3_FindFunction(&s_fn_main, s_runtime, "frame_app_main");
    if (r != m3Err_none || s_fn_main == NULL) {
        report("entry", "module does not export frame_app_main");
        return false;
    }
    if (m3_FindFunction(&s_fn_event, s_runtime, "frame_app_event") != m3Err_none) {
        s_fn_event = NULL;
    }
    if (m3_FindFunction(&s_fn_tick, s_runtime, "frame_app_tick") != m3Err_none) {
        s_fn_tick = NULL;
    }
    return true;
}

static void call_main(void)
{
    M3Result r = m3_CallV(s_fn_main);
    if (r != m3Err_none) {
        report("frame_app_main", r);
        s_stop_requested = true;
    }
}

static void call_event(int32_t type, int32_t a, int32_t b)
{
    if (s_fn_event == NULL) {
        return;
    }
    M3Result r = m3_CallV(s_fn_event, type, a, b);
    if (r != m3Err_none) {
        report("frame_app_event", r);
        s_stop_requested = true;
    }
}

static void call_tick(void)
{
    if (s_fn_tick == NULL) {
        return;
    }
    M3Result r = m3_CallV(s_fn_tick);
    if (r != m3Err_none) {
        report("frame_app_tick", r);
        s_stop_requested = true;
    }
}

static void back_to_launcher(void *arg)
{
    (void)arg;
    frame_wasm_request_stop();
}

static bool start_app(const char *app_id)
{
    const frame_app_state_t *app = frame_app_manager_find(app_id);
    if (app == NULL) {
        ESP_LOGW(TAG, "launch of unknown app %s", app_id);
        return false;
    }
    snprintf(s_app_id, sizeof(s_app_id), "%s", app_id);
    snprintf(s_app_name, sizeof(s_app_name), "%s", app->name);

    s_runtime = m3_NewRuntime(s_env, (uint32_t)CONFIG_FRAME_WASM_STACK_KB * 1024u, NULL);
    if (s_runtime == NULL) {
        report("runtime", "cannot allocate wasm stack");
        return false;
    }

    char path[FRAME_APP_PATH_MAX + 32];
    if (frame_store_managed_path(app_id, path, sizeof(path)) != ESP_OK || !load_module(path)) {
        runtime_release();
        return false;
    }

    ESP_LOGI(TAG, "starting %s %s (%u B linear memory)", app_id, app->version,
             (unsigned)s_linear_bytes);
    ui_tree_open(s_app_name, back_to_launcher, NULL);

    s_running        = true;
    s_stop_requested = false;

    call_main();

    frame_app_manager_mark_healthy(s_app_id);
    frame_app_manager_mark_launched(s_app_id, (uint32_t)(esp_timer_get_time() / 1000));
    return !s_stop_requested;
}

static void stop_app(void)
{
    s_running = false;
    ui_tree_close();
    runtime_release();
    s_app_id[0]   = '\0';
    s_app_name[0] = '\0';
    ESP_LOGI(TAG, "managed app stopped");
}

static void wasm_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wasm task on core %d", (int)xPortGetCoreID());

    wasm_msg_t m;
    for (;;) {
        const bool got = (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(100)) == pdTRUE);
        if (got) {
            if (m.kind == MSG_LAUNCH) {
                if (s_running) {
                    call_event(FRAME_WASM_EVT_STOP, 0, 0);
                    stop_app();
                }
                s_launch_result = start_app(m.app_id) ? 1 : -1;
            } else if (m.kind == MSG_STOP) {
                if (s_running) {
                    call_event(FRAME_WASM_EVT_STOP, 0, 0);
                    stop_app();
                }
            } else if (s_running) {
                call_event(m.type, m.a, m.b);
            }
        } else if (s_running) {
            call_tick();
        }

        if (s_stop_requested && s_running) {
            stop_app();
            frame_ui_home();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API (any task)                                               */
/* ------------------------------------------------------------------ */

static bool post(const wasm_msg_t *m)
{
    return (xQueueSend(s_queue, m, 0) == pdTRUE);
}

esp_err_t frame_wasm_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(16, sizeof(wasm_msg_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_env = m3_NewEnvironment();
    if (s_env == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ui_tree_init();
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        wasm_task, "frame_wasm", CONFIG_FRAME_WASM_TASK_STACK, NULL, 3, &s_task, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t frame_wasm_launch(const char *app_id)
{
    if (!s_inited || app_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Stop whatever is running first; the task serialises the two. */
    if (s_running) {
        (void)frame_wasm_stop();
    }

    wasm_msg_t m = { .kind = MSG_LAUNCH };
    snprintf(m.app_id, sizeof(m.app_id), "%s", app_id);
    s_launch_result = 0;
    if (!post(&m)) {
        return ESP_ERR_NO_MEM;
    }

    /* Wait for the task's verdict so the shell can report a load failure
     * synchronously and keep the launcher on screen when it fails. */
    for (uint32_t waited = 0; s_launch_result == 0 && waited < LAUNCH_TIMEOUT_MS; waited += 10u) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_launch_result == 0) {
        wasm_msg_t stop = { .kind = MSG_STOP };
        (void)post(&stop);
        return ESP_ERR_TIMEOUT;
    }
    return (s_launch_result > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t frame_wasm_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_running) {
        return ESP_OK;
    }
    (void)frame_wasm_request_stop();
    for (uint32_t waited = 0; s_running && waited < STOP_TIMEOUT_MS; waited += 10u) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_running ? ESP_ERR_TIMEOUT : ESP_OK;
}

void frame_wasm_request_stop(void)
{
    wasm_msg_t m = { .kind = MSG_STOP };
    (void)post(&m);
}

esp_err_t frame_wasm_post_event(int32_t type, int32_t a, int32_t b)
{
    if (!s_inited || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    wasm_msg_t m = { .kind = MSG_EVENT, .type = type, .a = a, .b = b };
    return post(&m) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool frame_wasm_running(void)
{
    return s_running;
}

const char *frame_wasm_current_app(void)
{
    return s_app_id;
}

void frame_wasm_get_memory(size_t *linear_bytes, size_t *stack_bytes)
{
    if (linear_bytes != NULL) {
        *linear_bytes = s_linear_bytes;
    }
    if (stack_bytes != NULL) {
        *stack_bytes = (size_t)CONFIG_FRAME_WASM_STACK_KB * 1024u;
    }
}
