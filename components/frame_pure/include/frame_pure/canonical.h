/*
 * frame_pure/canonical.h — deterministic JSON re-serialisation.
 *
 * A signature must verify on the device for a manifest that was signed on a
 * laptop, so both sides must agree on the exact bytes that are hashed. This
 * produces that canonical form: object keys sorted bytewise, no insignificant
 * whitespace, integers without a fractional part, and only the escapes JSON
 * actually requires.
 */
#pragma once

#include "frame_pure/json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} frame_canon_t;

void frame_canon_init(frame_canon_t *c, char *buf, size_t cap);

/* Appends a NUL-terminated canonical rendering. Returns false on overflow. */
bool frame_canon_write(frame_canon_t *c, const fj_val_t *v);

/* Canonicalises a JSON document: parse, then re-serialise canonically.
 * Returns false when the document is malformed or the output does not fit. */
bool frame_canonicalise(const char *json, size_t len, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
