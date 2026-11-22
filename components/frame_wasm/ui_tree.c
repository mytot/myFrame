/*
 * ui_tree.c — the retained-mode UI tree that managed apps emit, diffed into
 * LVGL on the LVGL task.
 *
 * Managed apps never touch LVGL. They describe what they want as a flat list
 * of operations (create node, set text, set style); the wasm task records
 * those into a bounded buffer during frame_ui_begin()..frame_ui_end(), and
 * the LVGL task applies the buffer in one go. That keeps the LVGL
 * single-thread rule intact and bounds the damage a buggy module can do: at
 * worst it produces a bad-looking page.
 */
#include "ui_tree.h"

#include "frame_ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wasm_ui";

#define UI_OPS_MAX    160u
#define UI_NODES_MAX  64u

typedef struct {
    uint8_t type;
    int32_t node;
    int32_t parent;
    int32_t ntype;
    char    a[48];
    char    b[40];
} ui_op_t;

typedef struct {
    int32_t   handle;
    lv_obj_t *obj;
    lv_obj_t *label;   /* buttons own a label; labels are their own */
    uint8_t   type;
} ui_node_t;

static ui_op_t  *s_ops;      /* PSRAM: 160 ops x ~100 B */
static size_t    s_op_count;
static bool      s_recording;

/* LVGL-side state: only touched on the LVGL task. */
static ui_node_t s_nodes[UI_NODES_MAX];
static lv_obj_t *s_root;
static bool      s_open;

esp_err_t ui_tree_init(void)
{
    if (s_ops != NULL) {
        return ESP_OK;
    }
    s_ops = heap_caps_malloc(sizeof(ui_op_t) * UI_OPS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ops == NULL) {
        s_ops = malloc(sizeof(ui_op_t) * UI_OPS_MAX);
    }
    return (s_ops != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (p != NULL) ? p : malloc(n);
}

static uint32_t parse_rgb(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    if (s[0] == '#') {
        s++;
    }
    return (uint32_t)strtoul(s, NULL, 16);
}

/* ------------------------------------------------------------------ */
/* Recording (wasm task)                                                */
/* ------------------------------------------------------------------ */

void ui_tree_reset(void)
{
    s_op_count = 0;
    s_recording = true;
}

static ui_op_t *op_alloc(void)
{
    if (!s_recording || s_ops == NULL || s_op_count >= UI_OPS_MAX) {
        return NULL;
    }
    ui_op_t *o = &s_ops[s_op_count++];
    memset(o, 0, sizeof(*o));
    return o;
}

void ui_tree_record_node(int32_t node, int32_t parent, int32_t type, const char *id)
{
    ui_op_t *o = op_alloc();
    if (o == NULL) {
        return;
    }
    o->type = UI_OP_NODE;
    o->node = node;
    o->parent = parent;
    o->ntype = type;
    snprintf(o->a, sizeof(o->a), "%s", id ? id : "");
}

void ui_tree_record_text(int32_t node, const char *text)
{
    ui_op_t *o = op_alloc();
    if (o == NULL) {
        return;
    }
    o->type = UI_OP_TEXT;
    o->node = node;
    snprintf(o->a, sizeof(o->a), "%s", text ? text : "");
}

void ui_tree_record_style(int32_t node, const char *prop, const char *value)
{
    ui_op_t *o = op_alloc();
    if (o == NULL) {
        return;
    }
    o->type = UI_OP_STYLE;
    o->node = node;
    snprintf(o->a, sizeof(o->a), "%s", prop ? prop : "");
    snprintf(o->b, sizeof(o->b), "%s", value ? value : "");
}

/* ------------------------------------------------------------------ */
/* Applying (LVGL task)                                                 */
/* ------------------------------------------------------------------ */

static ui_node_t *node_find(int32_t handle)
{
    for (size_t i = 0; i < UI_NODES_MAX; i++) {
        if (s_nodes[i].obj != NULL && s_nodes[i].handle == handle) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static ui_node_t *node_slot(void)
{
    for (size_t i = 0; i < UI_NODES_MAX; i++) {
        if (s_nodes[i].obj == NULL) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static void button_clicked(lv_event_t *e)
{
    const int32_t handle = (int32_t)(intptr_t)lv_event_get_user_data(e);
    (void)frame_wasm_post_event(FRAME_WASM_EVT_CLICK, handle, 0);
}

static lv_obj_t *parent_obj(int32_t parent)
{
    if (parent == 0) {
        return s_root;
    }
    ui_node_t *p = node_find(parent);
    return (p != NULL) ? p->obj : s_root;
}

static void create_node(int32_t handle, int32_t parent, int32_t type)
{
    if (s_root == NULL || handle == 0 || node_find(handle) != NULL) {
        return;
    }
    ui_node_t *n = node_slot();
    if (n == NULL) {
        ESP_LOGW(TAG, "node table full (> %u)", UI_NODES_MAX);
        return;
    }
    const frame_ui_theme_t *th = frame_ui_theme();
    lv_obj_t *parent_o = parent_obj(parent);
    lv_obj_t *obj = NULL;
    lv_obj_t *label = NULL;

    switch (type) {
    case FRAME_WASM_NODE_CONTAINER:
    case FRAME_WASM_NODE_ROW:
        obj = lv_obj_create(parent_o);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 8, 0);
        lv_obj_set_flex_flow(obj, (type == FRAME_WASM_NODE_ROW) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(obj, 8, 0);
        lv_obj_set_style_pad_column(obj, 8, 0);
        break;
    case FRAME_WASM_NODE_LABEL:
        obj = lv_label_create(parent_o);
        lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(th->text), 0);
        label = obj;
        break;
    case FRAME_WASM_NODE_BUTTON:
        obj = lv_button_create(parent_o);
        lv_obj_set_style_bg_color(obj, lv_color_hex(th->card), 0);
        lv_obj_set_style_radius(obj, 8, 0);
        lv_obj_set_style_pad_all(obj, 10, 0);
        label = lv_label_create(obj);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(th->text), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(obj, button_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)handle);
        break;
    case FRAME_WASM_NODE_PROGRESS:
        obj = lv_bar_create(parent_o);
        lv_obj_set_size(obj, 200, 12);
        lv_bar_set_range(obj, 0, 100);
        break;
    default:
        ESP_LOGW(TAG, "unknown node type %d", (int)type);
        return;
    }

    n->handle = handle;
    n->obj    = obj;
    n->label  = label;
    n->type   = (uint8_t)type;
}

static void set_text(int32_t handle, const char *text)
{
    ui_node_t *n = node_find(handle);
    if (n == NULL) {
        return;
    }
    if (n->type == FRAME_WASM_NODE_PROGRESS) {
        lv_bar_set_value(n->obj, (int32_t)strtol(text, NULL, 10), LV_ANIM_OFF);
        return;
    }
    if (n->label != NULL) {
        lv_label_set_text(n->label, text);
    }
}

static void set_style(int32_t handle, const char *prop, const char *value)
{
    ui_node_t *n = node_find(handle);
    if (n == NULL) {
        return;
    }
    lv_obj_t *o = n->obj;
    if (strcmp(prop, "text_font") == 0) {
        const lv_font_t *f = &lv_font_montserrat_14;
        if (strcmp(value, "20") == 0)      f = &lv_font_montserrat_20;
        else if (strcmp(value, "28") == 0) f = &lv_font_montserrat_28;
        else if (strcmp(value, "display") == 0) f = &lv_font_montserrat_28;
        if (n->label != NULL) {
            lv_obj_set_style_text_font(n->label, f, 0);
        } else if (n->type == FRAME_WASM_NODE_LABEL) {
            lv_obj_set_style_text_font(o, f, 0);
        }
    } else if (strcmp(prop, "text_color") == 0) {
        lv_obj_t *l = (n->label != NULL) ? n->label : o;
        lv_obj_set_style_text_color(l, lv_color_hex(parse_rgb(value)), 0);
    } else if (strcmp(prop, "bg") == 0) {
        lv_obj_set_style_bg_color(o, lv_color_hex(parse_rgb(value)), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    } else if (strcmp(prop, "pad") == 0) {
        lv_obj_set_style_pad_all(o, (int32_t)strtol(value, NULL, 10), 0);
    } else if (strcmp(prop, "width") == 0) {
        if (strcmp(value, "fill") == 0) lv_obj_set_width(o, LV_PCT(100));
        else lv_obj_set_width(o, (int32_t)strtol(value, NULL, 10));
    } else if (strcmp(prop, "height") == 0) {
        if (strcmp(value, "fill") == 0) lv_obj_set_height(o, LV_PCT(100));
        else lv_obj_set_height(o, (int32_t)strtol(value, NULL, 10));
    } else if (strcmp(prop, "radius") == 0) {
        lv_obj_set_style_radius(o, (int32_t)strtol(value, NULL, 10), 0);
    } else if (strcmp(prop, "align") == 0) {
        if (strcmp(value, "center") == 0)      lv_obj_align(o, LV_ALIGN_CENTER, 0, 0);
        else if (strcmp(value, "left") == 0)   lv_obj_align(o, LV_ALIGN_LEFT_MID, 0, 0);
        else if (strcmp(value, "right") == 0)  lv_obj_align(o, LV_ALIGN_RIGHT_MID, 0, 0);
        else if (strcmp(value, "top") == 0)    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 0);
        else if (strcmp(value, "bottom") == 0) lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else if (strcmp(prop, "text_align") == 0) {
        lv_text_align_t ta = LV_TEXT_ALIGN_AUTO;
        if (strcmp(value, "center") == 0)    ta = LV_TEXT_ALIGN_CENTER;
        else if (strcmp(value, "left") == 0) ta = LV_TEXT_ALIGN_LEFT;
        else if (strcmp(value, "right") == 0) ta = LV_TEXT_ALIGN_RIGHT;
        lv_obj_t *l = (n->label != NULL) ? n->label : o;
        lv_obj_set_style_text_align(l, ta, 0);
    } else if (strcmp(prop, "flex") == 0) {
        lv_obj_set_flex_flow(o, (strcmp(value, "row") == 0) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    } else if (strcmp(prop, "scroll") == 0) {
        if (strcmp(value, "on") == 0) lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        else lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
}

typedef struct {
    size_t count;
    ui_op_t ops[];
} ui_batch_t;

/* Runs on the LVGL task against a private copy, so the wasm task can start
 * recording the next frame the moment it has committed this one. */
static void apply_batch(void *arg)
{
    ui_batch_t *b = (ui_batch_t *)arg;
    if (s_open && s_root != NULL) {
        for (size_t i = 0; i < b->count; i++) {
            const ui_op_t *o = &b->ops[i];
            switch (o->type) {
            case UI_OP_NODE:  create_node(o->node, o->parent, o->ntype); break;
            case UI_OP_TEXT:  set_text(o->node, o->a); break;
            case UI_OP_STYLE: set_style(o->node, o->a, o->b); break;
            default: break;
            }
        }
    }
    free(b);
}

void ui_tree_commit(void)
{
    s_recording = false;
    if (s_op_count == 0u) {
        return;
    }
    ui_batch_t *b = psram_malloc(sizeof(ui_batch_t) + s_op_count * sizeof(ui_op_t));
    if (b == NULL) {
        ESP_LOGW(TAG, "dropped UI batch: no memory");
        return;
    }
    b->count = s_op_count;
    memcpy(b->ops, s_ops, s_op_count * sizeof(ui_op_t));
    /* Always through the dispatch queue: this runs on the wasm task, which
     * shares core 1 with LVGL but is not the LVGL task. */
    if (frame_ui_dispatch(apply_batch, b) != ESP_OK) {
        free(b);
    }
}

/* ------------------------------------------------------------------ */
/* Page lifecycle                                                       */
/* ------------------------------------------------------------------ */

static void (*s_on_back)(void *);
static void       *s_on_back_arg;

static void open_runner(void *arg)
{
    const char *title = (const char *)arg;
    memset(s_nodes, 0, sizeof(s_nodes));
    s_root = frame_ui_page(title ? title : "App");
    frame_ui_set_back(s_on_back, s_on_back_arg);
    /* A managed page is a column of content the module lays out itself. */
    if (s_root != NULL) {
        lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(s_root, 12, 0);
        lv_obj_set_style_pad_row(s_root, 8, 0);
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_root, LV_DIR_VER);
    }
    s_open = true;
}

void ui_tree_open(const char *title, void (*on_back)(void *), void *arg)
{
    s_on_back     = on_back;
    s_on_back_arg = arg;
    /* `title` must outlive the call: callers pass a static buffer and wait. */
    (void)frame_ui_dispatch_wait(open_runner, (void *)title, 2000);
}

static void close_runner(void *arg)
{
    (void)arg;
    memset(s_nodes, 0, sizeof(s_nodes));
    s_root = NULL;
    s_open = false;
}

void ui_tree_close(void)
{
    s_recording = false;
    s_op_count = 0;
    s_on_back  = NULL;
    (void)frame_ui_dispatch_wait(close_runner, NULL, 2000);
}
