/*
 * frame_pure/semver.h — semantic version parsing and range matching.
 *
 * Pure C, no ESP-IDF dependency, so it is unit-tested on the host and the same
 * binary runs on the device.
 *
 * Supported range syntax (a practical subset of node-semver):
 *   "*"                 any version
 *   "1.2.3"             exact
 *   "=1.2.3"            exact
 *   ">=1.2.0"           comparison operators
 *   "^1.2.3"            compatible: >=1.2.3 <2.0.0   (^0.2.3 -> >=0.2.3 <0.3.0)
 *   "~1.2.3"            approximately: >=1.2.3 <1.3.0
 *   ">=1.0.0 <2.0.0"    space-separated conjunction (AND)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_SEMVER_PRE_MAX 32u

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    char     pre[FRAME_SEMVER_PRE_MAX];  /* empty string when there is no prerelease */
} frame_semver_t;

/* Returns 0 on success, -1 on malformed input. */
int frame_semver_parse(const char *s, frame_semver_t *out);

/* Normalises a possibly-partial string ("1.2" -> 1.2.0, "2" -> 2.0.0). */
int frame_semver_parse_loose(const char *s, frame_semver_t *out);

/* -1 / 0 / +1. Prerelease sorts before the same release version. */
int frame_semver_cmp(const frame_semver_t *a, const frame_semver_t *b);

/* Formats "MAJOR.MINOR.PATCH[-pre]" into buf. Returns bytes written, -1 on overflow. */
int frame_semver_format(const frame_semver_t *v, char *buf, size_t cap);

/* True when v satisfies the range expression. An unparsable range is unsatisfiable. */
bool frame_semver_satisfies(const frame_semver_t *v, const char *range);

#ifdef __cplusplus
}
#endif
