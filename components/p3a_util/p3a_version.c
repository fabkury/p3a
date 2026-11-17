// SPDX-License-Identifier: Apache-2.0
#include "p3a_version.h"

#include <string.h>

// Reads a decimal number of 1 to 5 digits, at most 65535. Advances *p.
static bool read_part(const char **p, uint16_t *out)
{
    const char *s = *p;
    uint32_t value = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (uint32_t)(*s - '0');
        if (++digits > 5 || value > UINT16_MAX) {
            return false;
        }
        s++;
    }
    if (digits == 0) {
        return false;
    }
    *out = (uint16_t)value;
    *p = s;
    return true;
}

static bool suffix_char_ok(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
}

bool p3a_version_parse(const char *text, p3a_version_t *out)
{
    if (!text || !out) {
        return false;
    }
    const char *p = text;
    if (*p == 'v' || *p == 'V') {
        p++;
    }

    p3a_version_t v = {0};
    if (!read_part(&p, &v.major) || *p++ != '.') {
        return false;
    }
    if (!read_part(&p, &v.minor) || *p++ != '.') {
        return false;
    }
    if (!read_part(&p, &v.patch)) {
        return false;
    }

    if (*p == '-') {
        p++;
        size_t len = strlen(p);
        if (len == 0 || len >= P3A_VERSION_SUFFIX_MAX) {
            return false;
        }
        for (size_t i = 0; i < len; i++) {
            if (!suffix_char_ok(p[i])) {
                return false;
            }
        }
        memcpy(v.suffix, p, len + 1);
    } else if (*p != '\0') {
        return false;
    }

    *out = v;
    return true;
}

int p3a_version_compare(const p3a_version_t *a, const p3a_version_t *b)
{
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    bool a_release = a->suffix[0] == '\0';
    bool b_release = b->suffix[0] == '\0';
    if (a_release != b_release) {
        return a_release ? 1 : -1;
    }
    int c = strcmp(a->suffix, b->suffix);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}
