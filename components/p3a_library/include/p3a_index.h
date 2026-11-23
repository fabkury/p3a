// SPDX-License-Identifier: Apache-2.0
// The index: the records of one channel, in order, with a way to find one by
// its key and a note of which ones can be played now.
//
//   records     an array in PSRAM, in the channel's order
//   slot table  open addressing over id32, 16-bit positions: 16 KiB at 4096
//               records. Rebuilt from the records, never stored
//   available   one bit per record: its bytes are in the blob store. The
//               scheduler picks among these without touching the card
//
// 1.x kept two hash tables of individually allocated nodes per channel (about
// 512 KiB at 4096 entries, four times the entries themselves) and asked the
// card with stat() whether a file was there.
//
// An index is not safe for use from two tasks at once; its owner locks.
// p3a_index.c is pure (host-tested); p3a_index_file.c reads and writes files.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_INDEX_MAX_RECORDS 4096

typedef struct {
    p3a_record_t *records;
    uint32_t count;
    uint32_t capacity;
    uint16_t *slots;     // positions; P3A_INDEX_NO_SLOT when empty
    uint32_t slot_mask;  // number of slots - 1 (a power of two, at least twice the capacity)
    uint8_t *available;  // bitmap, one bit per position
    uint32_t available_count;
} p3a_index_t;

// Allocates an empty index for up to `capacity` records (1 to
// P3A_INDEX_MAX_RECORDS). ESP_ERR_INVALID_ARG or ESP_ERR_NO_MEM.
esp_err_t p3a_index_create(p3a_index_t *index, uint32_t capacity);

// Frees it. Safe on a zeroed or already destroyed index.
void p3a_index_destroy(p3a_index_t *index);

// Replaces the contents with `count` records (as read from a file). Every
// record is checked; one bad record refuses the lot (P3A_ERR_CORRUPT) and
// leaves the index empty. ESP_ERR_INVALID_SIZE if they do not fit. A record
// is marked available when its bytes are at hand (it has a blob, or is
// flagged in place) and it is not flagged gone.
esp_err_t p3a_index_set_records(p3a_index_t *index, const p3a_record_t *records, uint32_t count);

void p3a_index_clear(p3a_index_t *index);

// The position of the first record with this key, or -1.
int32_t p3a_index_find(const p3a_index_t *index, uint32_t id32);

// The position of the record that names the same artwork as `record` (same
// source and identity, not merely the same key), or -1.
int32_t p3a_index_find_record(const p3a_index_t *index, const p3a_record_t *record);

// Adds a record at the end. ESP_ERR_NO_MEM when the index is full,
// ESP_ERR_INVALID_STATE when the artwork is already in it, P3A_ERR_CORRUPT
// when the record does not pass p3a_record_check().
esp_err_t p3a_index_append(p3a_index_t *index, const p3a_record_t *record, uint32_t *position);

// Removes the record at `position`; later records move up by one, keeping
// their order. ESP_ERR_INVALID_ARG for a position past the end.
esp_err_t p3a_index_remove(p3a_index_t *index, uint32_t position);

// The record at `position`, or NULL past the end. The pointer is good until
// the next append, remove, set_records or clear. Do not change its source,
// identity or id32 through it: the slot table is keyed by them.
p3a_record_t *p3a_index_at(const p3a_index_t *index, uint32_t position);

// Whether the record at `position` can be played now. Setting it does not
// touch the record; the caller sets blob_ref.
bool p3a_index_is_available(const p3a_index_t *index, uint32_t position);
void p3a_index_set_available(p3a_index_t *index, uint32_t position, bool available);

// The position of the n-th available record (n from 0, in channel order), or
// -1 when fewer are available. This is how a pick lands on a record.
int32_t p3a_index_nth_available(const p3a_index_t *index, uint32_t n);

// Brings a fresh listing of a channel together with what the device already
// knew about that channel. `fresh` keeps its records and their order (the
// source's word on what the channel holds now); a record that `known` also
// has, by identity, takes over what was learned about it: its blob, its
// format, its size, its failure counts. Availability is then worked out
// anew. An artwork the source no longer lists is simply not in the result;
// its blob stays in the store until it ages out.
//
// 1.x had three near-copies of this, one per source, each with its own
// "evict what was not seen this refresh".
typedef struct {
    uint32_t carried;  // records known before
    uint32_t added;    // new in this listing
    uint32_t dropped;  // known before, no longer listed
} p3a_index_merge_t;
void p3a_index_carry_over(p3a_index_t *fresh, const p3a_index_t *known, p3a_index_merge_t *out);

// --- files (p3a_index_file.c) ----------------------------------------------------

// Saves the records as a framed container (p3a_frame.h). `flags` says what
// the file is: P3A_FRAME_REBUILDABLE for a cache of what a source lists,
// P3A_FRAME_USER_DATA for a list the user made. The slot table and the
// bitmap are not stored.
esp_err_t p3a_index_save(const p3a_index_t *index, const char *path, uint32_t flags);

// Loads a container saved by p3a_index_save() into `index`, which must have
// been created with room for it. Every result of p3a_frame_load() and
// p3a_index_set_records(); the file is never modified, whatever the result.
esp_err_t p3a_index_load(p3a_index_t *index, const char *path);

#ifdef __cplusplus
}
#endif
