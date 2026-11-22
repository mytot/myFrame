/*
 * frame_pure/json.h — a small, allocation-free JSON reader.
 *
 * Deliberately minimal: it parses a document into a tree allocated from a
 * caller-supplied arena, so it never calls malloc and never grows the heap.
 * Manifests are small (a few KB), which makes a bump arena exactly the right
 * tool: deterministic, fragmentation-free, and trivially host-testable.
 *
 * Not a general JSON library. Unsupported: UTF-16 escapes beyond the BMP
 * (they are rejected), duplicate-key detection, comments.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FJ_NULL,
    FJ_BOOL,
    FJ_NUM,
    FJ_STR,
    FJ_ARR,
    FJ_OBJ,
} fj_type_t;

typedef struct fj_val fj_val_t;

typedef struct {
    const char *key;   /* NUL-terminated, arena-owned */
    fj_val_t   *val;
} fj_pair_t;

struct fj_val {
    fj_type_t type;
    union {
        bool   b;
        double num;
        char  *str;                 /* NUL-terminated, arena-owned, escapes decoded */
        struct { fj_val_t **items; size_t count; } arr;
        struct { fj_pair_t *pairs; size_t count; } obj;
    } u;
};

/* ------------------------------------------------------------------ */
/* Arena                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   used;
} fj_arena_t;

#define FJ_ARENA_ALIGNMENT 8u

static inline void fj_arena_init(fj_arena_t *a, void *buf, size_t cap)
{
    a->buf  = (uint8_t *)buf;
    a->cap  = cap;
    a->used = 0;
}

/* Returns NULL when the arena is exhausted. */
void *fj_arena_alloc(fj_arena_t *a, size_t n);

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

#define FJ_MAX_DEPTH 24

/*
 * Parses `len` bytes of JSON. On success returns the root value; on failure
 * returns NULL and leaves the arena in an unspecified (but safe) state.
 *
 * The input buffer must outlive the returned tree for numeric literals; all
 * strings are copied into the arena.
 */
fj_val_t *fj_parse(const char *s, size_t len, fj_arena_t *arena);

/* ------------------------------------------------------------------ */
/* Accessors — all tolerate NULL inputs so callers can chain freely.     */
/* ------------------------------------------------------------------ */

fj_pair_t  *fj_obj_pair(const fj_val_t *obj, const char *key);
fj_val_t   *fj_obj_get(const fj_val_t *obj, const char *key);
const char *fj_str(const fj_val_t *v);          /* NULL when not a string */
bool        fj_as_bool(const fj_val_t *v, bool dflt);
long        fj_as_int(const fj_val_t *v, long dflt);
size_t      fj_arr_count(const fj_val_t *v);
fj_val_t   *fj_arr_at(const fj_val_t *v, size_t i);

/* Copies a string value into a fixed buffer. Returns false on overflow or type mismatch. */
bool fj_str_copy(const fj_val_t *v, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
