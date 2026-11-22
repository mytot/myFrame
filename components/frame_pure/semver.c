#include "frame_pure/semver.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static bool parse_num(const char **p, uint16_t *out)
{
    unsigned long v = 0;
    int digits = 0;
    while (isdigit((unsigned char)**p)) {
        v = v * 10u + (unsigned long)(**p - '0');
        if (v > 65535u) {
            return false;
        }
        (*p)++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        (*p)++;
    }
}

int frame_semver_parse(const char *s, frame_semver_t *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    /* An optional leading 'v' is tolerated. */
    if (*s == 'v') {
        s++;
    }

    const char *p = s;
    if (!parse_num(&p, &out->major)) {
        return -1;
    }
    if (*p++ != '.') {
        return -1;
    }
    if (!parse_num(&p, &out->minor)) {
        return -1;
    }
    if (*p++ != '.') {
        return -1;
    }
    if (!parse_num(&p, &out->patch)) {
        return -1;
    }

    if (*p == '-') {
        p++;
        size_t n = 0;
        while (*p != '\0' && *p != '+' && !isspace((unsigned char)*p)) {
            if (n + 1 >= sizeof(out->pre)) {
                return -1; /* prerelease too long */
            }
            out->pre[n++] = *p++;
        }
        out->pre[n] = '\0';
        if (n == 0) {
            return -1;
        }
    }

    /* Build metadata is accepted and discarded. */
    if (*p == '+') {
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
    }

    return (*p == '\0') ? 0 : -1;
}

int frame_semver_parse_loose(const char *s, frame_semver_t *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (*s == 'v') {
        s++;
    }

    const char *p = s;
    if (!parse_num(&p, &out->major)) {
        return -1;
    }
    if (*p == '.') {
        p++;
        if (!parse_num(&p, &out->minor)) {
            return -1;
        }
        if (*p == '.') {
            p++;
            if (!parse_num(&p, &out->patch)) {
                return -1;
            }
        }
    }
    return (*p == '\0') ? 0 : -1;
}

/* Prerelease identifiers compare per SemVer 2.0.0 §11.4:
 * numeric identifiers compare numerically, alphanumeric lexically, and a
 * shorter prefix sorts before a longer one. */
static int pre_cmp(const char *a, const char *b)
{
    if (a[0] == '\0' && b[0] == '\0') {
        return 0;
    }
    if (a[0] == '\0') {
        return 1;  /* a release > any prerelease */
    }
    if (b[0] == '\0') {
        return -1;
    }

    while (*a != '\0' || *b != '\0') {
        char ta[FRAME_SEMVER_PRE_MAX] = {0};
        char tb[FRAME_SEMVER_PRE_MAX] = {0};
        size_t i = 0;

        while (*a != '\0' && *a != '.' && i + 1 < sizeof(ta)) {
            ta[i++] = *a++;
        }
        while (*a == '.') {
            a++;
        }
        i = 0;
        while (*b != '\0' && *b != '.' && i + 1 < sizeof(tb)) {
            tb[i++] = *b++;
        }
        while (*b == '.') {
            b++;
        }

        bool na = ta[0] != '\0';
        bool nb = tb[0] != '\0';
        if (!na && !nb) {
            return 0;
        }
        if (!na) {
            return -1; /* fewer identifiers sorts first */
        }
        if (!nb) {
            return 1;
        }

        bool a_num = true, b_num = true;
        for (size_t k = 0; ta[k]; k++) {
            if (!isdigit((unsigned char)ta[k])) {
                a_num = false;
            }
        }
        for (size_t k = 0; tb[k]; k++) {
            if (!isdigit((unsigned char)tb[k])) {
                b_num = false;
            }
        }

        if (a_num && b_num) {
            unsigned long va = 0, vb = 0;
            for (size_t k = 0; ta[k]; k++) {
                va = va * 10u + (unsigned long)(ta[k] - '0');
            }
            for (size_t k = 0; tb[k]; k++) {
                vb = vb * 10u + (unsigned long)(tb[k] - '0');
            }
            if (va != vb) {
                return (va < vb) ? -1 : 1;
            }
        } else {
            int c = strcmp(ta, tb);
            if (c != 0) {
                return (c < 0) ? -1 : 1;
            }
        }
    }
    return 0;
}

int frame_semver_cmp(const frame_semver_t *a, const frame_semver_t *b)
{
    if (a->major != b->major) {
        return (a->major < b->major) ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return (a->minor < b->minor) ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return (a->patch < b->patch) ? -1 : 1;
    }
    return pre_cmp(a->pre, b->pre);
}

int frame_semver_format(const frame_semver_t *v, char *buf, size_t cap)
{
    if (v == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    int n;
    if (v->pre[0] != '\0') {
        n = snprintf(buf, cap, "%u.%u.%u-%s",
                     (unsigned)v->major, (unsigned)v->minor, (unsigned)v->patch, v->pre);
    } else {
        n = snprintf(buf, cap, "%u.%u.%u",
                     (unsigned)v->major, (unsigned)v->minor, (unsigned)v->patch);
    }
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/* Range matching                                                              */
/* -------------------------------------------------------------------------- */

typedef enum { OP_EQ, OP_LT, OP_LE, OP_GT, OP_GE, OP_ANY } cmp_op_t;

typedef struct {
    cmp_op_t       op;
    frame_semver_t ver;
} clause_t;

static bool parse_clause(const char **cursor, clause_t *out)
{
    skip_ws(cursor);
    if (**cursor == '\0') {
        return false;
    }

    const char *p = *cursor;
    out->op = OP_EQ;

    if (*p == '*') {
        out->op = OP_ANY;
        p++;
        memset(&out->ver, 0, sizeof(out->ver));
        *cursor = p;
        return true;
    }
    if (*p == '^' || *p == '~') {
        out->op = OP_GE;          /* the caret/tilde flag is read by the caller */
        p++;
    } else if (*p == '>') {
        p++;
        if (*p == '=') { out->op = OP_GE; p++; } else { out->op = OP_GT; }
    } else if (*p == '<') {
        p++;
        if (*p == '=') { out->op = OP_LE; p++; } else { out->op = OP_LT; }
    } else if (*p == '=') {
        out->op = OP_EQ;
        p++;
    }

    /* The version token ends at whitespace; frame_semver_parse rejects trailing
     * content, so the token must be isolated first. */
    char tok[64];
    size_t n = 0;
    while (*p != '\0' && !isspace((unsigned char)*p)) {
        if (n + 1u >= sizeof(tok)) {
            return false;
        }
        tok[n++] = *p++;
    }
    tok[n] = '\0';

    if (n == 0 || frame_semver_parse(tok, &out->ver) != 0) {
        return false;
    }
    *cursor = p;
    return true;
}

/* ^ and ~ need an upper bound, which a single comparison cannot express. They
 * are therefore expanded here into a lower/upper pair. */
static bool range_matches(const frame_semver_t *v, const char *range)
{
    if (range == NULL) {
        return false;
    }

    const char *p = range;

    for (;;) {
        skip_ws(&p);
        if (*p == '\0') {
            return true; /* all clauses satisfied */
        }

        const char *start = p;
        bool caret = (*p == '^');
        bool tilde = (*p == '~');

        clause_t c;
        if (!parse_clause(&p, &c)) {
            return false;
        }

        /* Reject garbage between clauses. */
        if (p == start) {
            return false;
        }

        if (c.op == OP_ANY) {
            continue;
        }

        int r = frame_semver_cmp(v, &c.ver);

        if (caret) {
            /* ^X.Y.Z  ->  >=X.Y.Z  and  <(X+1).0.0   (X==0: <0.(Y+1).0) */
            if (r < 0) {
                return false;
            }
            frame_semver_t upper = c.ver;
            if (upper.major == 0) {
                if (upper.minor == 0) {
                    upper.patch = (uint16_t)(upper.patch + 1u);
                } else {
                    upper.minor = (uint16_t)(upper.minor + 1u);
                    upper.patch = 0;
                }
            } else {
                upper.major = (uint16_t)(upper.major + 1u);
                upper.minor = 0;
                upper.patch = 0;
            }
            upper.pre[0] = '\0';
            if (frame_semver_cmp(v, &upper) >= 0) {
                return false;
            }
        } else if (tilde) {
            /* ~X.Y.Z  ->  >=X.Y.Z  and  <X.(Y+1).0 */
            if (r < 0) {
                return false;
            }
            frame_semver_t upper = c.ver;
            upper.minor = (uint16_t)(upper.minor + 1u);
            upper.patch = 0;
            upper.pre[0] = '\0';
            if (frame_semver_cmp(v, &upper) >= 0) {
                return false;
            }
        } else {
            switch (c.op) {
            case OP_EQ:
                if (r != 0) return false;
                break;
            case OP_LT:
                if (r >= 0) return false;
                break;
            case OP_LE:
                if (r > 0) return false;
                break;
            case OP_GT:
                if (r <= 0) return false;
                break;
            case OP_GE:
                if (r < 0) return false;
                break;
            default:
                break;
            }
        }
    }
}

bool frame_semver_satisfies(const frame_semver_t *v, const char *range)
{
    if (v == NULL) {
        return false;
    }
    if (range == NULL || range[0] == '\0') {
        return true; /* no constraint */
    }
    return range_matches(v, range);
}
