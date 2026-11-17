// SPDX-License-Identifier: Apache-2.0
// Firmware version strings: MAJOR.MINOR.PATCH with an optional -suffix,
// optionally prefixed with "v" (as in a release tag).
//
// Host-testable: no IDF includes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_VERSION_SUFFIX_MAX 24  // including the terminator

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    char suffix[P3A_VERSION_SUFFIX_MAX];  // "" for a release; "dev", "rc1", ... otherwise
} p3a_version_t;

// Parses "2.0.0", "v2.0.0", "2.0.0-dev". Returns false, leaving *out
// untouched, on anything else: a missing part, a part above 65535, trailing
// characters, an empty or over-long suffix, or a suffix with characters
// outside [A-Za-z0-9.-]. A string that does not parse has no version; callers
// must not treat it as equal to anything.
bool p3a_version_parse(const char *text, p3a_version_t *out);

// Orders two versions. Negative when a is older than b, zero when equal,
// positive when a is newer. Numbers compare first. With equal numbers, a
// version without a suffix is newer than one with a suffix (2.0.0 is newer
// than 2.0.0-dev), and two suffixes compare as plain strings.
int p3a_version_compare(const p3a_version_t *a, const p3a_version_t *b);

#ifdef __cplusplus
}
#endif
