#include "frame_pure/json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                                */
/* ------------------------------------------------------------------ */

void *fj_arena_alloc(fj_arena_t *a, size_t n)
{
    if (a == NULL || a->buf == NULL) {
        return NULL;
    }
    /* Align the current offset. */
    size_t off = (a->used + (FJ_ARENA_ALIGNMENT - 1u)) & ~(size_t)(FJ_ARENA_ALIGNMENT - 1u);
    if (off > a->cap || n > a->cap - off) {
        return NULL;
    }
    void *p = a->buf + off;
    a->used = off + n;
    return p;
}

/* ------------------------------------------------------------------ */
/* Parser                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    fj_arena_t *arena;
    int         depth;
} ps_t;

static void skip_ws(ps_t *s)
{
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            s->p++;
        } else {
            break;
        }
    }
}

static fj_val_t *parse_value(ps_t *s);

static bool hex4(const char *p, const char *end, unsigned *out)
{
    if (end - p < 4) {
        return false;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

/* Appends one UTF-8 code point into dst, advancing *w. */
static bool utf8_put(char *dst, size_t cap, size_t *w, unsigned cp)
{
    if (cp < 0x80u) {
        if (*w + 1 >= cap) return false;
        dst[(*w)++] = (char)cp;
        return true;
    }
    if (cp < 0x800u) {
        if (*w + 2 >= cap) return false;
        dst[(*w)++] = (char)(0xC0u | (cp >> 6));
        dst[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        return true;
    }
    if (cp < 0x10000u) {
        if (*w + 3 >= cap) return false;
        dst[(*w)++] = (char)(0xE0u | (cp >> 12));
        dst[(*w)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        return true;
    }
    if (cp < 0x110000u) {
        if (*w + 4 >= cap) return false;
        dst[(*w)++] = (char)(0xF0u | (cp >> 18));
        dst[(*w)++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[(*w)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        return true;
    }
    return false;
}

static char *parse_string(ps_t *s)
{
    if (s->p >= s->end || *s->p != '"') {
        return NULL;
    }
    s->p++;

    /* First pass: measure the decoded length so the arena allocation is exact. */
    const char *q = s->p;
    size_t need = 0;
    bool ok = true;
    while (q < s->end && *q != '"') {
        if (*q == '\\') {
            if (q + 1 >= s->end) { ok = false; break; }
            char e = q[1];
            q += 2;
            if (e == 'u') {
                unsigned cp;
                if (!hex4(q, s->end, &cp)) { ok = false; break; }
                q += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu && q + 6 <= s->end &&
                    q[0] == '\\' && q[1] == 'u') {
                    unsigned lo;
                    if (!hex4(q + 2, s->end, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                        ok = false;
                        break;
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    q += 6;
                }
                need += 4; /* worst case */
            } else {
                need += 1;
            }
        } else {
            need++;
            q++;
        }
    }
    if (!ok || q >= s->end) {
        return NULL;
    }

    char *dst = (char *)fj_arena_alloc(s->arena, need + 1u);
    if (dst == NULL) {
        return NULL;
    }

    size_t w = 0;
    while (s->p < s->end && *s->p != '"') {
        if (*s->p == '\\') {
            s->p++;
            if (s->p >= s->end) return NULL;
            char e = *s->p++;
            switch (e) {
            case '"':  dst[w++] = '"';  break;
            case '\\': dst[w++] = '\\'; break;
            case '/':  dst[w++] = '/';  break;
            case 'b':  dst[w++] = '\b'; break;
            case 'f':  dst[w++] = '\f'; break;
            case 'n':  dst[w++] = '\n'; break;
            case 'r':  dst[w++] = '\r'; break;
            case 't':  dst[w++] = '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(s->p, s->end, &cp)) return NULL;
                s->p += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu && s->p + 6 <= s->end &&
                    s->p[0] == '\\' && s->p[1] == 'u') {
                    unsigned lo;
                    if (!hex4(s->p + 2, s->end, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                        return NULL;
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    s->p += 6;
                }
                if (!utf8_put(dst, need + 1u, &w, cp)) return NULL;
                break;
            }
            default:
                return NULL;
            }
        } else {
            dst[w++] = *s->p++;
        }
    }
    dst[w] = '\0';
    s->p++; /* closing quote */
    return dst;
}

static fj_val_t *parse_value(ps_t *s)
{
    if (++s->depth > FJ_MAX_DEPTH) {
        return NULL;
    }
    skip_ws(s);
    if (s->p >= s->end) {
        s->depth--;
        return NULL;
    }

    fj_val_t *v = (fj_val_t *)fj_arena_alloc(s->arena, sizeof(fj_val_t));
    if (v == NULL) {
        s->depth--;
        return NULL;
    }
    memset(v, 0, sizeof(*v));

    char c = *s->p;

    if (c == '{') {
        v->type = FJ_OBJ;
        s->p++;
        skip_ws(s);
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            s->depth--;
            return v;
        }
        /* Grow the pair array in chunks of 8 to avoid a second pass. */
        size_t cap = 8;
        fj_pair_t *pairs = (fj_pair_t *)fj_arena_alloc(s->arena, cap * sizeof(fj_pair_t));
        if (pairs == NULL) { s->depth--; return NULL; }

        for (;;) {
            skip_ws(s);
            char *key = parse_string(s);
            if (key == NULL) { s->depth--; return NULL; }
            skip_ws(s);
            if (s->p >= s->end || *s->p != ':') { s->depth--; return NULL; }
            s->p++;
            fj_val_t *val = parse_value(s);
            if (val == NULL) { s->depth--; return NULL; }

            if (v->u.obj.count == cap) {
                size_t ncap = cap * 2u;
                fj_pair_t *np = (fj_pair_t *)fj_arena_alloc(s->arena, ncap * sizeof(fj_pair_t));
                if (np == NULL) { s->depth--; return NULL; }
                memcpy(np, pairs, cap * sizeof(fj_pair_t));
                pairs = np;
                cap  = ncap;
            }
            pairs[v->u.obj.count].key = key;
            pairs[v->u.obj.count].val = val;
            v->u.obj.count++;

            skip_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == '}') {
                s->p++;
                break;
            }
            s->depth--;
            return NULL;
        }
        v->u.obj.pairs = pairs;
        s->depth--;
        return v;
    }

    if (c == '[') {
        v->type = FJ_ARR;
        s->p++;
        skip_ws(s);
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            s->depth--;
            return v;
        }
        size_t cap = 8;
        fj_val_t **items = (fj_val_t **)fj_arena_alloc(s->arena, cap * sizeof(fj_val_t *));
        if (items == NULL) { s->depth--; return NULL; }

        for (;;) {
            fj_val_t *item = parse_value(s);
            if (item == NULL) { s->depth--; return NULL; }

            if (v->u.arr.count == cap) {
                size_t ncap = cap * 2u;
                fj_val_t **ni = (fj_val_t **)fj_arena_alloc(s->arena, ncap * sizeof(fj_val_t *));
                if (ni == NULL) { s->depth--; return NULL; }
                memcpy(ni, items, cap * sizeof(fj_val_t *));
                items = ni;
                cap   = ncap;
            }
            items[v->u.arr.count++] = item;

            skip_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == ']') {
                s->p++;
                break;
            }
            s->depth--;
            return NULL;
        }
        v->u.arr.items = items;
        s->depth--;
        return v;
    }

    if (c == '"') {
        char *str = parse_string(s);
        if (str == NULL) { s->depth--; return NULL; }
        v->type   = FJ_STR;
        v->u.str  = str;
        s->depth--;
        return v;
    }

    if (c == 't' && s->end - s->p >= 4 && memcmp(s->p, "true", 4) == 0) {
        v->type = FJ_BOOL;
        v->u.b  = true;
        s->p += 4;
        s->depth--;
        return v;
    }
    if (c == 'f' && s->end - s->p >= 5 && memcmp(s->p, "false", 5) == 0) {
        v->type = FJ_BOOL;
        v->u.b  = false;
        s->p += 5;
        s->depth--;
        return v;
    }
    if (c == 'n' && s->end - s->p >= 4 && memcmp(s->p, "null", 4) == 0) {
        v->type = FJ_NULL;
        s->p += 4;
        s->depth--;
        return v;
    }

    /* Number */
    {
        char *endp = NULL;
        double d = strtod(s->p, &endp);
        if (endp == s->p || endp > s->end || isnan(d) || isinf(d)) {
            s->depth--;
            return NULL;
        }
        v->type  = FJ_NUM;
        v->u.num = d;
        s->p = endp;
        s->depth--;
        return v;
    }
}

fj_val_t *fj_parse(const char *s, size_t len, fj_arena_t *arena)
{
    if (s == NULL || arena == NULL) {
        return NULL;
    }
    ps_t st = { .p = s, .end = s + len, .arena = arena, .depth = 0 };
    fj_val_t *root = parse_value(&st);
    if (root == NULL) {
        return NULL;
    }
    skip_ws(&st);
    if (st.p != st.end) {
        return NULL; /* trailing garbage */
    }
    return root;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                            */
/* ------------------------------------------------------------------ */

fj_pair_t *fj_obj_pair(const fj_val_t *obj, const char *key)
{
    if (obj == NULL || obj->type != FJ_OBJ || key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.pairs[i].key, key) == 0) {
            return &obj->u.obj.pairs[i];
        }
    }
    return NULL;
}

fj_val_t *fj_obj_get(const fj_val_t *obj, const char *key)
{
    fj_pair_t *p = fj_obj_pair(obj, key);
    return p ? p->val : NULL;
}

const char *fj_str(const fj_val_t *v)
{
    return (v != NULL && v->type == FJ_STR) ? v->u.str : NULL;
}

bool fj_as_bool(const fj_val_t *v, bool dflt)
{
    return (v != NULL && v->type == FJ_BOOL) ? v->u.b : dflt;
}

long fj_as_int(const fj_val_t *v, long dflt)
{
    return (v != NULL && v->type == FJ_NUM) ? (long)v->u.num : dflt;
}

size_t fj_arr_count(const fj_val_t *v)
{
    return (v != NULL && v->type == FJ_ARR) ? v->u.arr.count : 0u;
}

fj_val_t *fj_arr_at(const fj_val_t *v, size_t i)
{
    if (v == NULL || v->type != FJ_ARR || i >= v->u.arr.count) {
        return NULL;
    }
    return v->u.arr.items[i];
}

bool fj_str_copy(const fj_val_t *v, char *out, size_t cap)
{
    const char *s = fj_str(v);
    if (s == NULL || out == NULL || cap == 0) {
        return false;
    }
    size_t n = strlen(s);
    if (n >= cap) {
        return false;
    }
    memcpy(out, s, n + 1u);
    return true;
}
