// SPDX-License-Identifier: Apache-2.0
#include "p3a_index.h"

#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "psram_alloc.h"

#define P3A_INDEX_NO_SLOT 0xFFFFu

_Static_assert(P3A_INDEX_MAX_RECORDS < P3A_INDEX_NO_SLOT, "positions must fit a 16-bit slot");

static uint32_t slot_of(const p3a_index_t *index, uint32_t id32)
{
    // Fibonacci hashing: spreads keys that are sequential (Makapix post ids).
    return ((id32 * 2654435761u) >> 16) & index->slot_mask;  // the product's upper bits are the well mixed ones
}

static void slot_insert(p3a_index_t *index, uint32_t position)
{
    uint32_t slot = slot_of(index, index->records[position].id32);
    while (index->slots[slot] != P3A_INDEX_NO_SLOT) {
        slot = (slot + 1) & index->slot_mask;  // at most half full, so this ends
    }
    index->slots[slot] = (uint16_t)position;
}

static void rebuild_slots(p3a_index_t *index)
{
    memset(index->slots, 0xFF, (index->slot_mask + 1) * sizeof(uint16_t));
    for (uint32_t i = 0; i < index->count; i++) {
        slot_insert(index, i);
    }
}

static bool bit_get(const uint8_t *bits, uint32_t at)
{
    return (bits[at >> 3] >> (at & 7)) & 1;
}

static void bit_put(uint8_t *bits, uint32_t at, bool value)
{
    if (value) {
        bits[at >> 3] |= (uint8_t)(1u << (at & 7));
    } else {
        bits[at >> 3] &= (uint8_t)~(1u << (at & 7));
    }
}

esp_err_t p3a_index_create(p3a_index_t *index, uint32_t capacity)
{
    memset(index, 0, sizeof(*index));
    if (capacity == 0 || capacity > P3A_INDEX_MAX_RECORDS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t slots = 16;
    while (slots < capacity * 2) {
        slots *= 2;
    }
    index->records = psram_calloc(capacity, sizeof(p3a_record_t));
    index->slots = psram_malloc(slots * sizeof(uint16_t));
    index->available = psram_calloc((capacity + 7) / 8, 1);
    if (index->records == NULL || index->slots == NULL || index->available == NULL) {
        p3a_index_destroy(index);
        return ESP_ERR_NO_MEM;
    }
    index->capacity = capacity;
    index->slot_mask = slots - 1;
    memset(index->slots, 0xFF, slots * sizeof(uint16_t));
    return ESP_OK;
}

void p3a_index_destroy(p3a_index_t *index)
{
    free(index->records);
    free(index->slots);
    free(index->available);
    memset(index, 0, sizeof(*index));
}

void p3a_index_clear(p3a_index_t *index)
{
    index->count = 0;
    index->available_count = 0;
    memset(index->available, 0, (index->capacity + 7) / 8);
    memset(index->slots, 0xFF, (index->slot_mask + 1) * sizeof(uint16_t));
}

static bool playable_now(const p3a_record_t *record)
{
    return (p3a_record_has_blob(record) || (record->flags & P3A_RECORD_IN_PLACE) != 0) &&
           (record->flags & P3A_RECORD_GONE) == 0;
}

esp_err_t p3a_index_set_records(p3a_index_t *index, const p3a_record_t *records, uint32_t count)
{
    p3a_index_clear(index);
    if (count > index->capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (p3a_record_check(&records[i]) != ESP_OK) {
            return P3A_ERR_CORRUPT;
        }
    }
    if (count > 0) {
        memcpy(index->records, records, (size_t)count * sizeof(p3a_record_t));
    }
    index->count = count;
    rebuild_slots(index);
    for (uint32_t i = 0; i < count; i++) {
        if (playable_now(&index->records[i])) {
            bit_put(index->available, i, true);
            index->available_count++;
        }
    }
    return ESP_OK;
}

void p3a_index_carry_over(p3a_index_t *fresh, const p3a_index_t *known, p3a_index_merge_t *out)
{
    p3a_index_merge_t stats = {0};
    for (uint32_t i = 0; i < fresh->count; i++) {
        p3a_record_t *r = &fresh->records[i];
        const int32_t at = known != NULL && known->count > 0 ? p3a_index_find_record(known, r) : -1;
        if (at < 0) {
            stats.added++;
            continue;
        }
        stats.carried++;
        const p3a_record_t *old = &known->records[at];
        if (!p3a_record_has_blob(r) && p3a_record_has_blob(old)) {
            memcpy(r->blob_ref, old->blob_ref, sizeof(r->blob_ref));
            r->format = old->format;  // of the bytes that were fetched
            r->flags &= (uint8_t)~P3A_RECORD_UNRESOLVED;
        }
        if (r->width == 0 || r->height == 0) {
            r->width = old->width;
            r->height = old->height;
        }
        r->resolve_fails = old->resolve_fails;
        r->download_fails = old->download_fails;
    }
    stats.dropped = (known != NULL ? known->count : 0) - stats.carried;
    memset(fresh->available, 0, (fresh->capacity + 7) / 8);
    fresh->available_count = 0;
    for (uint32_t i = 0; i < fresh->count; i++) {
        if (playable_now(&fresh->records[i])) {
            bit_put(fresh->available, i, true);
            fresh->available_count++;
        }
    }
    if (out != NULL) {
        *out = stats;
    }
}

int32_t p3a_index_find(const p3a_index_t *index, uint32_t id32)
{
    if (index->slots == NULL) {
        return -1;
    }
    for (uint32_t slot = slot_of(index, id32); index->slots[slot] != P3A_INDEX_NO_SLOT;
         slot = (slot + 1) & index->slot_mask) {
        if (index->records[index->slots[slot]].id32 == id32) {
            return index->slots[slot];
        }
    }
    return -1;
}

int32_t p3a_index_find_record(const p3a_index_t *index, const p3a_record_t *record)
{
    if (index->slots == NULL) {
        return -1;
    }
    for (uint32_t slot = slot_of(index, record->id32); index->slots[slot] != P3A_INDEX_NO_SLOT;
         slot = (slot + 1) & index->slot_mask) {
        const p3a_record_t *held = &index->records[index->slots[slot]];
        if (held->id32 == record->id32 && p3a_record_same_identity(held, record)) {
            return index->slots[slot];
        }
    }
    return -1;
}

esp_err_t p3a_index_append(p3a_index_t *index, const p3a_record_t *record, uint32_t *position)
{
    if (p3a_record_check(record) != ESP_OK) {
        return P3A_ERR_CORRUPT;
    }
    if (p3a_index_find_record(index, record) >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index->count >= index->capacity) {
        return ESP_ERR_NO_MEM;
    }
    const uint32_t at = index->count++;
    index->records[at] = *record;
    slot_insert(index, at);
    const bool available = playable_now(record);
    bit_put(index->available, at, available);
    index->available_count += available ? 1 : 0;
    if (position) {
        *position = at;
    }
    return ESP_OK;
}

esp_err_t p3a_index_remove(p3a_index_t *index, uint32_t position)
{
    if (position >= index->count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bit_get(index->available, position)) {
        index->available_count--;
    }
    const uint32_t after = index->count - position - 1;
    memmove(&index->records[position], &index->records[position + 1], (size_t)after * sizeof(p3a_record_t));
    for (uint32_t i = position; i + 1 < index->count; i++) {
        bit_put(index->available, i, bit_get(index->available, i + 1));
    }
    index->count--;
    bit_put(index->available, index->count, false);
    // Every later position changed: the table is rebuilt (a few hundred
    // microseconds at 4096 records), which is simpler than fixing it up and
    // leaves no deleted-slot markers behind.
    rebuild_slots(index);
    return ESP_OK;
}

p3a_record_t *p3a_index_at(const p3a_index_t *index, uint32_t position)
{
    return position < index->count ? &index->records[position] : NULL;
}

bool p3a_index_is_available(const p3a_index_t *index, uint32_t position)
{
    return position < index->count && bit_get(index->available, position);
}

void p3a_index_set_available(p3a_index_t *index, uint32_t position, bool available)
{
    if (position >= index->count || bit_get(index->available, position) == available) {
        return;
    }
    bit_put(index->available, position, available);
    index->available_count += available ? 1u : (uint32_t)-1;
}

int32_t p3a_index_nth_available(const p3a_index_t *index, uint32_t n)
{
    if (n >= index->available_count) {
        return -1;
    }
    // Whole bytes first: at 4096 records this is at most 512 steps.
    static const uint8_t ones[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    uint32_t byte = 0;
    for (;; byte++) {
        const uint8_t bits = index->available[byte];
        const uint32_t here = (uint32_t)ones[bits & 15] + ones[bits >> 4];
        if (n < here) {
            break;
        }
        n -= here;
    }
    for (uint32_t bit = 0; bit < 8; bit++) {
        if ((index->available[byte] >> bit) & 1) {
            if (n == 0) {
                return (int32_t)(byte * 8 + bit);
            }
            n--;
        }
    }
    return -1;  // not reached while available_count matches the bitmap
}
