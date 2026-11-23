// SPDX-License-Identifier: Apache-2.0
// Which blobs go when the card fills up. See p3a_evict.h. Pure.

#include "p3a_evict.h"

#include <stdlib.h>
#include <string.h>

#define MIB (1024ull * 1024)

static uint64_t clamp(uint64_t v, uint64_t lo, uint64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

bool p3a_evict_marks(uint64_t total_bytes, uint64_t *start_bytes, uint64_t *stop_bytes)
{
    *start_bytes = 0;
    *stop_bytes = 0;
    if (total_bytes < P3A_EVICT_MIN_CARD) {
        return false;
    }
    *start_bytes = clamp(total_bytes / 10, 256 * MIB, 1024 * MIB);
    *stop_bytes = *start_bytes + clamp(total_bytes / 4, 512 * MIB, 4096 * MIB);
    return true;
}

static uint32_t s_cutoff;  // qsort has no context argument; one caller at a time (the content task)

static bool spared(const p3a_evict_entry_t *e)
{
    return e->keep || e->modified_at > s_cutoff;
}

static int by_age(const void *pa, const void *pb)
{
    const p3a_evict_entry_t *a = pa, *b = pb;
    const bool sa = spared(a), sb = spared(b);
    if (sa != sb) {
        return sa ? 1 : -1;  // what may go comes first
    }
    if (a->modified_at != b->modified_at) {
        return a->modified_at < b->modified_at ? -1 : 1;
    }
    return memcmp(a->ref, b->ref, sizeof(a->ref));  // a fixed order among equals
}

uint32_t p3a_evict_choose(p3a_evict_entry_t *entries, uint32_t count, uint64_t need_bytes, uint32_t now,
                          uint32_t min_age_s, uint64_t *freed)
{
    // Younger than the minimum age, or dated after now (a clock that was
    // wrong): spared.
    s_cutoff = now >= min_age_s ? now - min_age_s : 0;
    if (count > 1) {
        qsort(entries, count, sizeof(entries[0]), by_age);
    }
    uint64_t total = 0;
    uint32_t n = 0;
    while (n < count && total < need_bytes && !spared(&entries[n])) {
        total += entries[n].size;
        n++;
    }
    if (freed != NULL) {
        *freed = total;
    }
    return n;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;  // the store writes lower case; anything else is not its file
}

bool p3a_evict_parse_name(const char *name, uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t *format)
{
    static const struct {
        const char *ext;
        p3a_format_t format;
    } known[] = {{".webp", P3A_FORMAT_WEBP}, {".gif", P3A_FORMAT_GIF}, {".png", P3A_FORMAT_PNG},
                 {".jpg", P3A_FORMAT_JPEG},  {".bmp", P3A_FORMAT_BMP}};
    if (name == NULL || strlen(name) < 2 * P3A_RECORD_BLOB_REF_LEN + 4) {
        return false;
    }
    bool any = false;
    for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
        const int hi = hex_digit(name[2 * i]), lo = hex_digit(name[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        ref[i] = (uint8_t)(hi << 4 | lo);
        any = any || ref[i] != 0;
    }
    const char *ext = name + 2 * P3A_RECORD_BLOB_REF_LEN;
    for (size_t i = 0; any && i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(ext, known[i].ext) == 0) {
            *format = known[i].format;
            return true;
        }
    }
    return false;
}
