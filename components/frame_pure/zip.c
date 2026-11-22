#include "frame_pure/zip.h"

#include <stdlib.h>
#include <string.h>

#define SIG_LOCAL      0x04034b50u
#define SIG_CENTRAL    0x02014b50u
#define SIG_EOCD       0x06054b50u
#define EOCD_MIN_LEN   22u
#define EOCD_MAX_SCAN  65557u   /* EOCD + max comment */
#define LOCAL_HDR_LEN  30u
#define CENTRAL_HDR_LEN 46u

struct frame_zip {
    FILE *f;
    frame_zip_entry_t *entries;
    size_t count;
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int read_exact(FILE *f, void *buf, size_t len)
{
    return (fread(buf, 1, len, f) == len) ? FRAME_ZIP_OK : FRAME_ZIP_ERR_IO;
}

static int seek_to(FILE *f, long off)
{
    return (fseek(f, off, SEEK_SET) == 0) ? FRAME_ZIP_OK : FRAME_ZIP_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3, reflected) — the polynomial ZIP uses.           */
/* ------------------------------------------------------------------ */

static uint32_t s_crc_table[256];
static int      s_crc_ready;

static void crc_init(void)
{
    if (s_crc_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t len)
{
    crc_init();
    crc ^= 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc = s_crc_table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

/* ------------------------------------------------------------------ */

static int find_eocd(FILE *f, long *cd_offset_out, uint16_t *count_out)
{
    if (seek_to(f, 0) != FRAME_ZIP_OK || fseek(f, 0, SEEK_END) != 0) {
        return FRAME_ZIP_ERR_IO;
    }
    const long size = ftell(f);
    if (size < (long)EOCD_MIN_LEN) {
        return FRAME_ZIP_ERR_FORMAT;
    }

    const long scan = (size < (long)EOCD_MAX_SCAN) ? size : (long)EOCD_MAX_SCAN;
    uint8_t *buf = malloc((size_t)scan);
    if (buf == NULL) {
        return FRAME_ZIP_ERR_NOMEM;
    }
    if (seek_to(f, size - scan) != FRAME_ZIP_OK ||
        read_exact(f, buf, (size_t)scan) != FRAME_ZIP_OK) {
        free(buf);
        return FRAME_ZIP_ERR_IO;
    }

    long eocd = -1;
    /* Scan backwards so a ZIP stored inside a comment cannot fool us. */
    for (long i = scan - (long)EOCD_MIN_LEN; i >= 0; i--) {
        if (rd32(buf + i) == SIG_EOCD) {
            /* The comment length must reach exactly to the end of file. */
            const uint16_t comment = rd16(buf + i + 20);
            if ((long)(i + EOCD_MIN_LEN + comment) == scan) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd < 0) {
        free(buf);
        return FRAME_ZIP_ERR_FORMAT;
    }

    const uint16_t count = rd16(buf + eocd + 10);
    const uint32_t cd_size = rd32(buf + eocd + 12);
    const uint32_t cd_off = rd32(buf + eocd + 16);
    free(buf);

    /* ZIP64 markers: unsupported, and no package should need them. */
    if (count == 0xffffu || cd_size == 0xffffffffu || cd_off == 0xffffffffu) {
        return FRAME_ZIP_ERR_FORMAT;
    }
    if (count > FRAME_ZIP_MAX_ENTRIES) {
        return FRAME_ZIP_ERR_RANGE;
    }
    if ((long)(cd_off + cd_size) > size - (long)EOCD_MIN_LEN + 1) {
        return FRAME_ZIP_ERR_FORMAT;
    }

    *cd_offset_out = (long)cd_off;
    *count_out = count;
    return FRAME_ZIP_OK;
}

frame_zip_t *frame_zip_open(FILE *f)
{
    if (f == NULL) {
        return NULL;
    }
    long cd_off = 0;
    uint16_t count = 0;
    const int rc = find_eocd(f, &cd_off, &count);
    if (rc != FRAME_ZIP_OK) {
        return NULL;
    }

    frame_zip_t *z = calloc(1, sizeof(*z));
    if (z == NULL) {
        return NULL;
    }
    z->f = f;
    z->count = count;
    if (count > 0u) {
        z->entries = calloc(count, sizeof(frame_zip_entry_t));
        if (z->entries == NULL) {
            free(z);
            return NULL;
        }
    }

    if (seek_to(f, cd_off) != FRAME_ZIP_OK) {
        frame_zip_close(z);
        return NULL;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint8_t hdr[CENTRAL_HDR_LEN];
        if (read_exact(f, hdr, sizeof(hdr)) != FRAME_ZIP_OK || rd32(hdr) != SIG_CENTRAL) {
            frame_zip_close(z);
            return NULL;
        }
        const uint16_t name_len = rd16(hdr + 28);
        const uint16_t extra_len = rd16(hdr + 30);
        const uint16_t comment_len = rd16(hdr + 32);

        frame_zip_entry_t *e = &z->entries[i];
        e->method = rd16(hdr + 10);
        e->crc32 = rd32(hdr + 16);
        e->comp_size = rd32(hdr + 20);
        e->uncomp_size = rd32(hdr + 24);
        const uint32_t local_off = rd32(hdr + 42);

        uint8_t name[FRAME_ZIP_NAME_MAX];
        const size_t copy = (name_len < FRAME_ZIP_NAME_MAX) ? name_len : FRAME_ZIP_NAME_MAX - 1u;
        if (copy > 0u && read_exact(f, name, copy) != FRAME_ZIP_OK) {
            frame_zip_close(z);
            return NULL;
        }
        name[copy] = '\0';
        memcpy(e->name, name, copy + 1u);

        if (name_len > copy && seek_to(f, ftell(f) + (long)(name_len - copy)) != FRAME_ZIP_OK) {
            frame_zip_close(z);
            return NULL;
        }
        if (seek_to(f, ftell(f) + (long)extra_len + (long)comment_len) != FRAME_ZIP_OK) {
            frame_zip_close(z);
            return NULL;
        }

        /* Resolve the member data offset through the local header, whose
         * name/extra lengths can differ from the central record. */
        uint8_t local[LOCAL_HDR_LEN];
        const long here = ftell(f);
        if (seek_to(f, (long)local_off) != FRAME_ZIP_OK ||
            read_exact(f, local, sizeof(local)) != FRAME_ZIP_OK ||
            rd32(local) != SIG_LOCAL) {
            frame_zip_close(z);
            return NULL;
        }
        e->data_offset = local_off + LOCAL_HDR_LEN + rd16(local + 26) + rd16(local + 28);
        if (seek_to(f, here) != FRAME_ZIP_OK) {
            frame_zip_close(z);
            return NULL;
        }
    }

    return z;
}

void frame_zip_close(frame_zip_t *z)
{
    if (z == NULL) {
        return;
    }
    free(z->entries);
    free(z);
}

size_t frame_zip_count(const frame_zip_t *z)
{
    return (z != NULL) ? z->count : 0u;
}

const frame_zip_entry_t *frame_zip_at(const frame_zip_t *z, size_t index)
{
    if (z == NULL || index >= z->count) {
        return NULL;
    }
    return &z->entries[index];
}

const frame_zip_entry_t *frame_zip_find(const frame_zip_t *z, const char *name)
{
    if (z == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < z->count; i++) {
        if (strncmp(z->entries[i].name, name, FRAME_ZIP_NAME_MAX) == 0) {
            return &z->entries[i];
        }
    }
    return NULL;
}

long frame_zip_stream(const frame_zip_t *z, const frame_zip_entry_t *e,
                      int (*sink)(void *ctx, const uint8_t *data, size_t len, void *user),
                      void *ctx)
{
    if (z == NULL || e == NULL || sink == NULL) {
        return FRAME_ZIP_ERR_NOTFOUND;
    }
    if (e->method != 0u) {
        return FRAME_ZIP_ERR_DEFLATE;
    }
    if (e->comp_size != e->uncomp_size) {
        return FRAME_ZIP_ERR_FORMAT;
    }
    if (seek_to(z->f, (long)e->data_offset) != FRAME_ZIP_OK) {
        return FRAME_ZIP_ERR_IO;
    }

    uint8_t *buf = malloc(FRAME_ZIP_CHUNK);
    if (buf == NULL) {
        return FRAME_ZIP_ERR_NOMEM;
    }

    uint32_t crc = 0;
    uint32_t left = e->comp_size;
    while (left > 0u) {
        const size_t take = (left > FRAME_ZIP_CHUNK) ? FRAME_ZIP_CHUNK : left;
        if (read_exact(z->f, buf, take) != FRAME_ZIP_OK) {
            free(buf);
            return FRAME_ZIP_ERR_IO;
        }
        crc = crc_update(crc, buf, take);
        if (sink(ctx, buf, take, (void *)e) != 0) {
            free(buf);
            return FRAME_ZIP_ERR_IO;
        }
        left -= (uint32_t)take;
    }
    free(buf);

    if (crc != e->crc32) {
        return FRAME_ZIP_ERR_CRC;
    }
    return (long)e->uncomp_size;
}

static int sink_to_buffer(void *ctx, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    uint8_t **cursor = (uint8_t **)ctx;
    memcpy(*cursor, data, len);
    *cursor += len;
    return 0;
}

long frame_zip_read(const frame_zip_t *z, const frame_zip_entry_t *e,
                    uint8_t *buf, size_t cap)
{
    if (z == NULL || e == NULL || buf == NULL) {
        return FRAME_ZIP_ERR_NOTFOUND;
    }
    if (e->uncomp_size > cap) {
        return FRAME_ZIP_ERR_RANGE;
    }
    uint8_t *cursor = buf;
    return frame_zip_stream(z, e, sink_to_buffer, &cursor);
}
