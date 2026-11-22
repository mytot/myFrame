/*
 * frame_pure/manifest.h — .fpack manifest parsing and requirement matching.
 *
 * A manifest is the contract between a publisher and a device. Everything the
 * device needs to decide "can I install this, and should I?" is derived here,
 * before a single byte is written to flash.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_MANIFEST_MAX_PERMISSIONS 8u
#define FRAME_MANIFEST_MAX_CHIPS       4u

typedef enum {
    FRAME_MANIFEST_NATIVE = 0,
    FRAME_MANIFEST_MANAGED = 1,
} frame_manifest_kind_t;

typedef enum {
    FRAME_RENDER_STATIC = 0,
    FRAME_RENDER_LIGHT = 1,
    FRAME_RENDER_MEDIUM = 2,
    FRAME_RENDER_FULL = 3,
} frame_render_class_t;

/* Result codes. */
#define FRAME_MANIFEST_OK            0
#define FRAME_MANIFEST_ERR_PARSE    (-1)
#define FRAME_MANIFEST_ERR_SCHEMA   (-2)
#define FRAME_MANIFEST_ERR_FIELD    (-3)
#define FRAME_MANIFEST_ERR_TOO_LONG (-4)
#define FRAME_MANIFEST_ERR_UNKNOWN  (-5)

/* Requirement mismatches. */
#define FRAME_REQ_OK              0
#define FRAME_REQ_ERR_CHIP      (-10)
#define FRAME_REQ_ERR_PSRAM      (-11)
#define FRAME_REQ_ERR_DISPLAY    (-12)
#define FRAME_REQ_ERR_TOUCH      (-13)
#define FRAME_REQ_ERR_AUDIO      (-14)
#define FRAME_REQ_ERR_SDK        (-15)

typedef struct {
    uint32_t                schema;
    char                    id[64];
    char                    name[48];
    char                    version[24];
    frame_manifest_kind_t   kind;
    char                    sdk[32];

    struct {
        char     chips[FRAME_MANIFEST_MAX_CHIPS][16];
        uint8_t  chip_count;
        int32_t  psram_mb;
        uint16_t min_width;
        uint16_t min_height;
        bool     touch;
        bool     audio;
    } requires;

    char     permissions[FRAME_MANIFEST_MAX_PERMISSIONS][24];
    uint8_t  permission_count;

    char     entry[48];
    uint32_t size;
    char     sha256[65];        /* lowercase hex */
    char     description[192];
    char     author[48];
    char     homepage[96];
    char     release_notes[192];

    struct {
        frame_render_class_t cls;
        uint8_t              max_fps;
        bool                 direct;
    } render;
} frame_manifest_t;

/* What the running device offers, for requirement matching. */
typedef struct {
    const char *chip;
    uint32_t    psram_mb;
    uint16_t    width;
    uint16_t    height;
    bool        touch;
    bool        audio;
    const char *sdk_version;
} frame_device_caps_t;

/* ------------------------------------------------------------------ */

const char *frame_manifest_kind_str(frame_manifest_kind_t k);
const char *frame_render_class_str(frame_render_class_t c);
int         frame_render_class_from_str(const char *s, frame_render_class_t *out);

/*
 * Parses and validates a manifest. Returns FRAME_MANIFEST_OK or a negative
 * error code. The arena is caller-supplied so no heap allocation occurs.
 */
int frame_manifest_parse(const char *json, size_t len, frame_manifest_t *out);

/*
 * Checks a parsed manifest against device capabilities.
 * Returns FRAME_REQ_OK or a negative FRAME_REQ_ERR_* code.
 */
int frame_manifest_check_requirements(const frame_manifest_t *m, const frame_device_caps_t *caps);

/* True when the manifest declares the given permission. */
bool frame_manifest_has_permission(const frame_manifest_t *m, const char *perm);

#ifdef __cplusplus
}
#endif
