// SPDX-License-Identifier: Apache-2.0
// What the museums' files share, and the table of museums. See museum.h. Pure.

#include "museum.h"

#include <stdio.h>
#include <string.h>

static const museum_t *const s_museums[] = {
    &museum_vam,
    &museum_smk,
};

const museum_t *museum_at(size_t index)
{
    return index < sizeof(s_museums) / sizeof(s_museums[0]) ? s_museums[index] : NULL;
}

const museum_t *museum_of_kind(const char *kind, const char **axis)
{
    const char *colon = kind != NULL ? strchr(kind, ':') : NULL;
    if (colon == NULL) {
        return NULL;
    }
    const size_t id_len = (size_t)(colon - kind);
    for (size_t i = 0; museum_at(i) != NULL; i++) {
        const museum_t *m = museum_at(i);
        if (strlen(m->id) == id_len && strncmp(m->id, kind, id_len) == 0) {
            if (!m->axis_ok(colon + 1)) {
                return NULL;
            }
            if (axis != NULL) {
                *axis = colon + 1;
            }
            return m;
        }
    }
    return NULL;
}

const museum_t *museum_of_record(const p3a_record_t *record)
{
    if (record == NULL || record->source != P3A_SOURCE_MUSEUM) {
        return NULL;
    }
    for (size_t i = 0; museum_at(i) != NULL; i++) {
        if (museum_at(i)->number == record->identity.museum.museum_id) {
            return museum_at(i);
        }
    }
    return NULL;
}

bool museum_put(char *out, size_t len, size_t *at, const char *text)
{
    const size_t n = strlen(text);
    if (*at + n >= len) {
        return false;
    }
    memcpy(out + *at, text, n + 1);
    *at += n;
    return true;
}

bool museum_put_encoded(char *out, size_t len, size_t *at, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        const unsigned char c = *p;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
                           c == '_' || c == '.' || c == '~';
        char piece[4] = {0};
        if (plain) {
            piece[0] = (char)c;
        } else {
            piece[0] = '%';
            piece[1] = hex[c >> 4];
            piece[2] = hex[c & 15];
        }
        if (!museum_put(out, len, at, piece)) {
            return false;
        }
    }
    return true;
}

bool museum_put_number(char *out, size_t len, size_t *at, uint32_t n)
{
    char text[12];
    snprintf(text, sizeof(text), "%u", (unsigned)n);
    return museum_put(out, len, at, text);
}

bool museum_key_ok(const char *key)
{
    if (key == NULL) {
        return false;
    }
    const size_t len = strlen(key);
    if (len == 0 || len >= P3A_RECORD_IIIF_KEY_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = key[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
              c == '.' || c == '~')) {
            return false;
        }
    }
    // Not a way up the path, whatever the server would make of it.
    return strcmp(key, ".") != 0 && strcmp(key, "..") != 0;
}

void museum_page_add(museum_page_t *page, uint16_t museum, const char *key, uint32_t width, uint32_t height)
{
    if (page->count >= page->room || !museum_key_ok(key)) {
        return;
    }
    p3a_record_t *r = &page->out[page->count];
    if (p3a_record_init_museum(r, museum, key) != ESP_OK) {
        return;
    }
    r->sort_at = page->now;
    if (width > 0 && height > 0 && width <= 100000 && height <= 100000) {
        // The picture asked for is the original fitted into a square of
        // MUSEUM_LONGEST_SIDE, never enlarged.
        const uint32_t longest = width > height ? width : height;
        if (longest > MUSEUM_LONGEST_SIDE) {
            width = width * MUSEUM_LONGEST_SIDE / longest;
            height = height * MUSEUM_LONGEST_SIDE / longest;
        }
        r->width = (uint16_t)(width > 0 ? width : 1);
        r->height = (uint16_t)(height > 0 ? height : 1);
    }
    page->count++;
}

esp_err_t museum_iiif_url(const char *base, const p3a_record_t *record, char *out, size_t len)
{
    if (len == 0 || !museum_key_ok(record->identity.museum.iiif_key)) {
        return ESP_ERR_INVALID_ARG;
    }
    char size[40];
    size_t at = 0;
    out[0] = 0;
    snprintf(size, sizeof(size), "/full/!%d,%d/0/default.jpg", MUSEUM_LONGEST_SIDE, MUSEUM_LONGEST_SIDE);
    return museum_put(out, len, &at, base) && museum_put(out, len, &at, "/") &&
                   museum_put(out, len, &at, record->identity.museum.iiif_key) && museum_put(out, len, &at, size)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}
