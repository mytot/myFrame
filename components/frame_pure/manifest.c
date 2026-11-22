#include "frame_pure/manifest.h"

#include "frame_pure/json.h"
#include "frame_pure/semver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parsed manifests live in a fixed arena: they are a few KB at most and this
 * keeps the install path entirely free of heap allocation. */
#define MANIFEST_ARENA_BYTES 6144u

const char *frame_manifest_kind_str(frame_manifest_kind_t k)
{
    return (k == FRAME_MANIFEST_MANAGED) ? "managed" : "native";
}

const char *frame_render_class_str(frame_render_class_t c)
{
    switch (c) {
    case FRAME_RENDER_STATIC: return "static";
    case FRAME_RENDER_LIGHT:  return "light";
    case FRAME_RENDER_MEDIUM: return "medium";
    case FRAME_RENDER_FULL:   return "full";
    default:                  return "static";
    }
}

int frame_render_class_from_str(const char *s, frame_render_class_t *out)
{
    if (s == NULL || out == NULL) {
        return FRAME_MANIFEST_ERR_FIELD;
    }
    if (strcmp(s, "static") == 0) { *out = FRAME_RENDER_STATIC; return FRAME_MANIFEST_OK; }
    if (strcmp(s, "light")  == 0) { *out = FRAME_RENDER_LIGHT;  return FRAME_MANIFEST_OK; }
    if (strcmp(s, "medium") == 0) { *out = FRAME_RENDER_MEDIUM; return FRAME_MANIFEST_OK; }
    if (strcmp(s, "full")   == 0) { *out = FRAME_RENDER_FULL;   return FRAME_MANIFEST_OK; }
    return FRAME_MANIFEST_ERR_FIELD;
}

/* Copies a JSON string into a fixed buffer. Returns false on type mismatch or
 * overflow — a truncated id would be a silent security bug, so overflow is
 * always an error. */
static bool copy_str(const fj_val_t *parent, const char *key, char *dst, size_t cap,
                     bool required, int *err)
{
    const fj_val_t *v = fj_obj_get(parent, key);
    if (v == NULL) {
        if (required && err != NULL && *err == FRAME_MANIFEST_OK) {
            *err = FRAME_MANIFEST_ERR_FIELD;
        }
        return false;
    }
    const char *s = fj_str(v);
    if (s == NULL) {
        if (err != NULL && *err == FRAME_MANIFEST_OK) {
            *err = FRAME_MANIFEST_ERR_FIELD;
        }
        return false;
    }
    if (strlen(s) >= cap) {
        if (err != NULL && *err == FRAME_MANIFEST_OK) {
            *err = FRAME_MANIFEST_ERR_TOO_LONG;
        }
        return false;
    }
    memcpy(dst, s, strlen(s) + 1u);
    return true;
}

int frame_manifest_parse(const char *json, size_t len, frame_manifest_t *out)
{
    if (json == NULL || out == NULL) {
        return FRAME_MANIFEST_ERR_PARSE;
    }
    memset(out, 0, sizeof(*out));
    out->kind         = FRAME_MANIFEST_NATIVE;
    out->render.cls   = FRAME_RENDER_LIGHT;
    out->render.max_fps = 30;

    static uint8_t arena_buf[MANIFEST_ARENA_BYTES];
    fj_arena_t arena;
    fj_arena_init(&arena, arena_buf, sizeof(arena_buf));

    fj_val_t *root = fj_parse(json, len, &arena);
    if (root == NULL || root->type != FJ_OBJ) {
        return FRAME_MANIFEST_ERR_PARSE;
    }

    int err = FRAME_MANIFEST_OK;

    /* --- required scalars ------------------------------------------- */
    const fj_val_t *schema = fj_obj_get(root, "schema");
    if (schema == NULL || schema->type != FJ_NUM) {
        return FRAME_MANIFEST_ERR_FIELD;
    }
    out->schema = (uint32_t)fj_as_int(schema, 0);
    if (out->schema != 1u) {
        return FRAME_MANIFEST_ERR_SCHEMA;
    }

    copy_str(root, "id", out->id, sizeof(out->id), true, &err);
    copy_str(root, "name", out->name, sizeof(out->name), true, &err);
    copy_str(root, "version", out->version, sizeof(out->version), true, &err);
    copy_str(root, "sdk", out->sdk, sizeof(out->sdk), true, &err);
    copy_str(root, "entry", out->entry, sizeof(out->entry), true, &err);
    copy_str(root, "sha256", out->sha256, sizeof(out->sha256), true, &err);
    if (err != FRAME_MANIFEST_OK) {
        return err;
    }

    /* A malformed version would break every comparison downstream. */
    {
        frame_semver_t v;
        if (frame_semver_parse(out->version, &v) != 0) {
            return FRAME_MANIFEST_ERR_FIELD;
        }
    }
    if (strlen(out->sha256) != 64u) {
        return FRAME_MANIFEST_ERR_FIELD;
    }

    out->size = (uint32_t)fj_as_int(fj_obj_get(root, "size"), 0);

    /* --- kind -------------------------------------------------------- */
    {
        const char *k = fj_str(fj_obj_get(root, "kind"));
        if (k == NULL) {
            return FRAME_MANIFEST_ERR_FIELD;
        }
        if (strcmp(k, "native") == 0) {
            out->kind = FRAME_MANIFEST_NATIVE;
        } else if (strcmp(k, "managed") == 0) {
            out->kind = FRAME_MANIFEST_MANAGED;
        } else {
            return FRAME_MANIFEST_ERR_UNKNOWN;
        }
    }

    /* --- requires ---------------------------------------------------- */
    const fj_val_t *req = fj_obj_get(root, "requires");
    if (req != NULL && req->type == FJ_OBJ) {
        out->requires.psram_mb = (int32_t)fj_as_int(fj_obj_get(req, "psram_mb"), 0);

        const fj_val_t *chips = fj_obj_get(req, "chip");
        if (chips != NULL && chips->type == FJ_ARR) {
            size_t n = fj_arr_count(chips);
            if (n > FRAME_MANIFEST_MAX_CHIPS) {
                n = FRAME_MANIFEST_MAX_CHIPS;
            }
            for (size_t i = 0; i < n; i++) {
                const char *c = fj_str(fj_arr_at(chips, i));
                if (c == NULL || strlen(c) >= sizeof(out->requires.chips[0])) {
                    return FRAME_MANIFEST_ERR_FIELD;
                }
                memcpy(out->requires.chips[i], c, strlen(c) + 1u);
                out->requires.chip_count++;
            }
        }

        const fj_val_t *disp = fj_obj_get(req, "display");
        if (disp != NULL && disp->type == FJ_OBJ) {
            out->requires.min_width  = (uint16_t)fj_as_int(fj_obj_get(disp, "min_width"), 0);
            out->requires.min_height = (uint16_t)fj_as_int(fj_obj_get(disp, "min_height"), 0);
            out->requires.touch      = fj_as_bool(fj_obj_get(disp, "touch"), false);
        }
        out->requires.audio = fj_as_bool(fj_obj_get(req, "audio"), false);
    }

    /* --- permissions ------------------------------------------------- */
    const fj_val_t *perms = fj_obj_get(root, "permissions");
    if (perms != NULL && perms->type == FJ_ARR) {
        size_t n = fj_arr_count(perms);
        for (size_t i = 0; i < n; i++) {
            const char *p = fj_str(fj_arr_at(perms, i));
            if (p == NULL) {
                return FRAME_MANIFEST_ERR_FIELD;
            }
            if (out->permission_count >= FRAME_MANIFEST_MAX_PERMISSIONS) {
                break; /* excess permissions are ignored, not fatal */
            }
            if (strlen(p) >= sizeof(out->permissions[0])) {
                return FRAME_MANIFEST_ERR_TOO_LONG;
            }
            memcpy(out->permissions[out->permission_count++], p, strlen(p) + 1u);
        }
    }

    /* --- render class (optional) ------------------------------------- */
    const fj_val_t *render = fj_obj_get(root, "render");
    if (render != NULL && render->type == FJ_OBJ) {
        const char *cls = fj_str(fj_obj_get(render, "class"));
        if (cls != NULL) {
            frame_render_class_t c;
            if (frame_render_class_from_str(cls, &c) != FRAME_MANIFEST_OK) {
                return FRAME_MANIFEST_ERR_FIELD;
            }
            out->render.cls = c;
        }
        out->render.max_fps = (uint8_t)fj_as_int(fj_obj_get(render, "max_fps"), 30);
        if (out->render.max_fps == 0u) {
            out->render.max_fps = 30u;
        }
        out->render.direct = fj_as_bool(fj_obj_get(render, "direct"), false);
    }

    /* --- descriptive fields (optional) -------------------------------- */
    copy_str(root, "description", out->description, sizeof(out->description), false, &err);
    copy_str(root, "author", out->author, sizeof(out->author), false, &err);
    copy_str(root, "homepage", out->homepage, sizeof(out->homepage), false, &err);
    copy_str(root, "release_notes", out->release_notes, sizeof(out->release_notes), false, &err);
    /* Optional fields that are merely too long are truncated rather than fatal. */
    err = FRAME_MANIFEST_OK;

    return err;
}

int frame_manifest_check_requirements(const frame_manifest_t *m, const frame_device_caps_t *caps)
{
    if (m == NULL || caps == NULL) {
        return FRAME_REQ_ERR_CHIP;
    }

    if (m->requires.chip_count > 0u) {
        bool ok = false;
        for (uint8_t i = 0; i < m->requires.chip_count; i++) {
            if (strcmp(m->requires.chips[i], caps->chip) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return FRAME_REQ_ERR_CHIP;
        }
    }

    if (m->requires.psram_mb > 0 && (int32_t)caps->psram_mb < m->requires.psram_mb) {
        return FRAME_REQ_ERR_PSRAM;
    }
    if (m->requires.min_width > 0u && caps->width < m->requires.min_width) {
        return FRAME_REQ_ERR_DISPLAY;
    }
    if (m->requires.min_height > 0u && caps->height < m->requires.min_height) {
        return FRAME_REQ_ERR_DISPLAY;
    }
    if (m->requires.touch && !caps->touch) {
        return FRAME_REQ_ERR_TOUCH;
    }
    if (m->requires.audio && !caps->audio) {
        return FRAME_REQ_ERR_AUDIO;
    }
    if (caps->sdk_version != NULL && m->sdk[0] != '\0') {
        frame_semver_t have;
        if (frame_semver_parse_loose(caps->sdk_version, &have) != 0 ||
            !frame_semver_satisfies(&have, m->sdk)) {
            return FRAME_REQ_ERR_SDK;
        }
    }
    return FRAME_REQ_OK;
}

bool frame_manifest_has_permission(const frame_manifest_t *m, const char *perm)
{
    if (m == NULL || perm == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < m->permission_count; i++) {
        if (strcmp(m->permissions[i], perm) == 0) {
            return true;
        }
    }
    return false;
}
