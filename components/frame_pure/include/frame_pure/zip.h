/*
 * frame_pure/zip.h — a minimal ZIP reader over a stdio stream.
 *
 * .fpack is a ZIP container (see docs/APP_STORE.md §2) so that a package can
 * be inspected with `unzip -l` on any laptop. This reader is deliberately
 * small: central-directory parsing, STORED entries, CRC-32 checking, and
 * streaming a member to a sink in bounded chunks so a 1 MB native image never
 * has to exist in RAM at once.
 *
 * Deflate (method 8) is NOT supported: packages are built with `--store`.
 * Compressing an ESP image gains little, and an inflate implementation is
 * more code than the rest of this file. A deflated member is reported as
 * FRAME_ZIP_ERR_DEFLATE so the device can say so instead of failing oddly.
 *
 * No ZIP64: a package will never approach 4 GB.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_ZIP_NAME_MAX     64u
#define FRAME_ZIP_MAX_ENTRIES  64u
#define FRAME_ZIP_CHUNK        8192u

#define FRAME_ZIP_OK             0
#define FRAME_ZIP_ERR_IO       (-1)
#define FRAME_ZIP_ERR_FORMAT   (-2)
#define FRAME_ZIP_ERR_NOMEM    (-3)
#define FRAME_ZIP_ERR_NOTFOUND (-4)
#define FRAME_ZIP_ERR_DEFLATE  (-5)
#define FRAME_ZIP_ERR_CRC      (-6)
#define FRAME_ZIP_ERR_RANGE    (-7)

typedef struct {
    char     name[FRAME_ZIP_NAME_MAX];
    uint16_t method;          /* 0 = stored, 8 = deflate */
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t crc32;
    uint32_t data_offset;     /* absolute file offset of the member data */
} frame_zip_entry_t;

typedef struct frame_zip frame_zip_t;

/* Reads the central directory. Takes ownership of nothing: `f` stays open. */
frame_zip_t *frame_zip_open(FILE *f);
void         frame_zip_close(frame_zip_t *z);

size_t frame_zip_count(const frame_zip_t *z);
const frame_zip_entry_t *frame_zip_at(const frame_zip_t *z, size_t index);
const frame_zip_entry_t *frame_zip_find(const frame_zip_t *z, const char *name);

/*
 * Streams a member to `sink` in FRAME_ZIP_CHUNK pieces and verifies the CRC.
 * Returns the uncompressed length or a negative FRAME_ZIP_ERR_* code. A sink
 * that returns non-zero aborts with FRAME_ZIP_ERR_IO.
 */
long frame_zip_stream(const frame_zip_t *z, const frame_zip_entry_t *e,
                      int (*sink)(void *ctx, const uint8_t *data, size_t len, void *user),
                      void *ctx);

/* Convenience: reads a small member fully into buf. */
long frame_zip_read(const frame_zip_t *z, const frame_zip_entry_t *e,
                    uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
