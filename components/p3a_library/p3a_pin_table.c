// SPDX-License-Identifier: Apache-2.0
// The table of pin lists and the set of kept blobs. See p3a_pins.h. Pure.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "p3a_pins.h"
#include "psram_alloc.h"

bool p3a_pin_name_ok(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t len = strlen(name);
    if (len == 0 || len >= P3A_PIN_NAME_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool p3a_pin_slug_ok(const char *slug)
{
    if (slug == NULL || strlen(slug) != P3A_PIN_SLUG_LEN - 1) {
        return false;
    }
    return strspn(slug, "0123456789abcdef") == P3A_PIN_SLUG_LEN - 1;
}

void p3a_pin_table_init(p3a_pin_table_t *table)
{
    memset(table, 0, sizeof(*table));
}

static int index_of(const p3a_pin_table_t *table, const char *slug)
{
    if (!p3a_pin_slug_ok(slug)) {
        return -1;
    }
    for (uint32_t i = 0; i < table->count; i++) {
        if (memcmp(table->lists[i].slug, slug, P3A_PIN_SLUG_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool entry_ok(const p3a_pin_list_t *e)
{
    static const uint8_t zero[sizeof(e->reserved)] = {0};
    // Terminated inside their fields, with nothing after the terminator.
    const char *slug_end = memchr(e->slug, 0, sizeof(e->slug));
    const char *name_end = memchr(e->name, 0, sizeof(e->name));
    if (slug_end == NULL || name_end == NULL || !p3a_pin_slug_ok(e->slug) || !p3a_pin_name_ok(e->name)) {
        return false;
    }
    for (const char *p = name_end; p < e->name + sizeof(e->name); p++) {
        if (*p != 0) {
            return false;
        }
    }
    return e->active <= 1 && memcmp(e->reserved, zero, sizeof(zero)) == 0;
}

esp_err_t p3a_pin_table_set(p3a_pin_table_t *table, const p3a_pin_list_t *lists, uint32_t count)
{
    p3a_pin_table_init(table);
    if (count > P3A_PINS_MAX_LISTS) {
        return P3A_ERR_CORRUPT;
    }
    uint32_t active = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!entry_ok(&lists[i])) {
            return P3A_ERR_CORRUPT;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (memcmp(lists[i].slug, lists[j].slug, P3A_PIN_SLUG_LEN) == 0) {
                return P3A_ERR_CORRUPT;
            }
        }
        active += lists[i].active;
    }
    if (count > 0) {
        memcpy(table->lists, lists, (size_t)count * sizeof(p3a_pin_list_t));
    }
    table->count = count;
    if (active != 1) {
        for (uint32_t i = 0; i < count; i++) {
            table->lists[i].active = i == 0;
        }
    }
    return ESP_OK;
}

esp_err_t p3a_pin_table_create(p3a_pin_table_t *table, const char *name, uint32_t salt, char slug[P3A_PIN_SLUG_LEN])
{
    if (!p3a_pin_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (table->count >= P3A_PINS_MAX_LISTS) {
        return ESP_ERR_NO_MEM;
    }
    // FNV-1a over the name and the salt; stepped until it is no other list's.
    uint32_t h = 2166136261u ^ salt;
    for (const unsigned char *p = (const unsigned char *)name; *p != 0; p++) {
        h = (h ^ *p) * 16777619u;
    }
    char candidate[P3A_PIN_SLUG_LEN];
    do {
        snprintf(candidate, sizeof(candidate), "%08x", (unsigned)h);
        h = h * 16777619u + 1;
    } while (index_of(table, candidate) >= 0);
    p3a_pin_list_t *e = &table->lists[table->count];
    memset(e, 0, sizeof(*e));
    memcpy(e->slug, candidate, sizeof(candidate));
    memcpy(e->name, name, strlen(name));
    e->active = table->count == 0;
    table->count++;
    memcpy(slug, candidate, sizeof(candidate));
    return ESP_OK;
}

esp_err_t p3a_pin_table_rename(p3a_pin_table_t *table, const char *slug, const char *name)
{
    const int at = index_of(table, slug);
    if (at < 0) {
        return P3A_ERR_MISSING;
    }
    if (!p3a_pin_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(table->lists[at].name, 0, sizeof(table->lists[at].name));
    memcpy(table->lists[at].name, name, strlen(name));
    return ESP_OK;
}

esp_err_t p3a_pin_table_set_active(p3a_pin_table_t *table, const char *slug)
{
    const int at = index_of(table, slug);
    if (at < 0) {
        return P3A_ERR_MISSING;
    }
    for (uint32_t i = 0; i < table->count; i++) {
        table->lists[i].active = (int)i == at;
    }
    return ESP_OK;
}

esp_err_t p3a_pin_table_delete(p3a_pin_table_t *table, const char *slug)
{
    const int at = index_of(table, slug);
    if (at < 0) {
        return P3A_ERR_MISSING;
    }
    if (table->lists[at].active) {
        return ESP_ERR_INVALID_STATE;
    }
    memmove(&table->lists[at], &table->lists[at + 1], (table->count - (uint32_t)at - 1) * sizeof(p3a_pin_list_t));
    table->count--;
    memset(&table->lists[table->count], 0, sizeof(p3a_pin_list_t));
    return ESP_OK;
}

const p3a_pin_list_t *p3a_pin_table_find(const p3a_pin_table_t *table, const char *slug)
{
    const int at = index_of(table, slug);
    return at >= 0 ? &table->lists[at] : NULL;
}

const p3a_pin_list_t *p3a_pin_table_active(const p3a_pin_table_t *table)
{
    for (uint32_t i = 0; i < table->count; i++) {
        if (table->lists[i].active) {
            return &table->lists[i];
        }
    }
    return NULL;
}

// --- the kept blobs -------------------------------------------------------------

static bool is_zero(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
        if (ref[i] != 0) {
            return false;
        }
    }
    return true;
}

// The first position whose reference is not below `ref`.
static uint32_t lower_bound(const p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    uint32_t lo = 0, hi = keep->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (memcmp(keep->refs[mid], ref, P3A_RECORD_BLOB_REF_LEN) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

esp_err_t p3a_pin_keep_add(p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    if (is_zero(ref)) {
        return ESP_OK;
    }
    if (keep->count == keep->capacity) {
        const uint32_t capacity = keep->capacity == 0 ? 256 : keep->capacity * 2;
        void *grown = psram_malloc((size_t)capacity * P3A_RECORD_BLOB_REF_LEN);
        if (grown == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (keep->count > 0) {
            memcpy(grown, keep->refs, (size_t)keep->count * P3A_RECORD_BLOB_REF_LEN);
        }
        free(keep->refs);
        keep->refs = grown;
        keep->capacity = capacity;
    }
    const uint32_t at = lower_bound(keep, ref);
    memmove(keep->refs[at + 1], keep->refs[at], (size_t)(keep->count - at) * P3A_RECORD_BLOB_REF_LEN);
    memcpy(keep->refs[at], ref, P3A_RECORD_BLOB_REF_LEN);
    keep->count++;
    return ESP_OK;
}

void p3a_pin_keep_remove(p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    const uint32_t at = lower_bound(keep, ref);
    if (at < keep->count && memcmp(keep->refs[at], ref, P3A_RECORD_BLOB_REF_LEN) == 0) {
        memmove(keep->refs[at], keep->refs[at + 1], (size_t)(keep->count - at - 1) * P3A_RECORD_BLOB_REF_LEN);
        keep->count--;
    }
}

bool p3a_pin_keep_has(const p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    const uint32_t at = lower_bound(keep, ref);
    return at < keep->count && memcmp(keep->refs[at], ref, P3A_RECORD_BLOB_REF_LEN) == 0;
}

void p3a_pin_keep_free(p3a_pin_keep_t *keep)
{
    free(keep->refs);
    memset(keep, 0, sizeof(*keep));
}
