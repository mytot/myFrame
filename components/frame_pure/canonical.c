#include "frame_pure/canonical.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void put(frame_canon_t *c, char ch)
{
    if (c->overflow) {
        return;
    }
    if (c->len + 1u >= c->cap) {
        c->overflow = true;
        return;
    }
    c->buf[c->len++] = ch;
    c->buf[c->len]   = '\0';
}

static void put_str(frame_canon_t *c, const char *s)
{
    put(c, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  put(c, '\\'); put(c, '"');  break;
        case '\\': put(c, '\\'); put(c, '\\'); break;
        case '\b': put(c, '\\'); put(c, 'b');  break;
        case '\f': put(c, '\\'); put(c, 'f');  break;
        case '\n': put(c, '\\'); put(c, 'n');  break;
        case '\r': put(c, '\\'); put(c, 'r');  break;
        case '\t': put(c, '\\'); put(c, 't');  break;
        default:
            if (*p < 0x20u) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                for (char *q = esc; *q; q++) {
                    put(c, *q);
                }
            } else {
                put(c, (char)*p);
            }
        }
    }
    put(c, '"');
}

/* Bytewise key ordering, as required for deterministic serialisation. */
static int key_cmp(const void *a, const void *b)
{
    const fj_pair_t *pa = (const fj_pair_t *)a;
    const fj_pair_t *pb = (const fj_pair_t *)b;
    return strcmp(pa->key, pb->key);
}

static void write_value(frame_canon_t *c, const fj_val_t *v, int depth)
{
    if (v == NULL || depth > FJ_MAX_DEPTH || c->overflow) {
        if (!c->overflow) {
            c->overflow = true;
        }
        return;
    }

    switch (v->type) {
    case FJ_NULL:
        for (const char *p = "null"; *p; p++) put(c, *p);
        break;

    case FJ_BOOL:
        for (const char *p = v->u.b ? "true" : "false"; *p; p++) put(c, *p);
        break;

    case FJ_NUM: {
        char tmp[40];
        double d = v->u.num;
        if (isnan(d) || isinf(d)) {
            c->overflow = true;
            return;
        }
        /* Integral values are written without a decimal point so that 421376
         * and 421376.0 produce identical bytes. */
        if (d == floor(d) && fabs(d) < 9.0e15) {
            snprintf(tmp, sizeof(tmp), "%lld", (long long)d);
        } else {
            snprintf(tmp, sizeof(tmp), "%.17g", d);
        }
        for (char *p = tmp; *p; p++) put(c, *p);
        break;
    }

    case FJ_STR:
        put_str(c, v->u.str);
        break;

    case FJ_ARR:
        put(c, '[');
        for (size_t i = 0; i < v->u.arr.count; i++) {
            if (i > 0) put(c, ',');
            write_value(c, v->u.arr.items[i], depth + 1);
        }
        put(c, ']');
        break;

    case FJ_OBJ: {
        size_t n = v->u.obj.count;
        if (n == 0) {
            put(c, '{');
            put(c, '}');
            break;
        }

        /* Sort a copy of the pair index; the arena-owned array stays intact. */
        fj_pair_t *sorted = NULL;
        size_t bytes = n * sizeof(fj_pair_t);
        /* Small object counts dominate manifests; fall back to insertion sort
         * in place on a stack buffer to avoid allocating. */
        fj_pair_t stackbuf[32];
        fj_pair_t *tmp = (n <= 32u) ? stackbuf : NULL;
        if (tmp == NULL) {
            c->overflow = true;
            return;
        }
        memcpy(tmp, v->u.obj.pairs, bytes);
        sorted = tmp;

        for (size_t i = 1; i < n; i++) {
            fj_pair_t key = sorted[i];
            size_t j = i;
            while (j > 0 && key_cmp(&sorted[j - 1], &key) > 0) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = key;
        }

        put(c, '{');
        for (size_t i = 0; i < n; i++) {
            if (i > 0) put(c, ',');
            put_str(c, sorted[i].key);
            put(c, ':');
            write_value(c, sorted[i].val, depth + 1);
        }
        put(c, '}');
        break;
    }

    default:
        c->overflow = true;
        break;
    }
}

void frame_canon_init(frame_canon_t *c, char *buf, size_t cap)
{
    c->buf      = buf;
    c->cap      = cap;
    c->len      = 0;
    c->overflow = (buf == NULL || cap == 0);
    if (!c->overflow) {
        c->buf[0] = '\0';
    }
}

bool frame_canon_write(frame_canon_t *c, const fj_val_t *v)
{
    write_value(c, v, 0);
    return !c->overflow;
}

bool frame_canonicalise(const char *json, size_t len, char *out, size_t out_cap)
{
    /* Manifests are small; a fixed 8 KB arena is ample and keeps this
     * allocation-free. */
    static uint8_t arena_buf[8192];
    fj_arena_t arena;
    fj_arena_init(&arena, arena_buf, sizeof(arena_buf));

    fj_val_t *root = fj_parse(json, len, &arena);
    if (root == NULL) {
        return false;
    }

    frame_canon_t c;
    frame_canon_init(&c, out, out_cap);
    return frame_canon_write(&c, root);
}
