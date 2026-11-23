// SPDX-License-Identifier: Apache-2.0
// Making room on the card: which blobs go when it fills up.
//
// The blob store is a cache of what the sources serve. When free space
// falls below a mark, the oldest blobs are deleted until there is room
// again. Two marks, so that one download does not cross back and forth:
//
//   start   10% of the card, at least 256 MiB, at most 1 GiB free
//   stop    start + 25% of the card, at least 512 MiB, at most 4 GiB
//
// A card under 1 GiB is not evicted from at all: the marks would take most
// of it, and downloads simply stop when it is full.
//
// What is never deleted: a blob younger than four hours (a card too small
// for its playset would otherwise fetch and delete in a circle), and a blob
// the caller says to keep (the one on the panel; the user's pins).
//
// A blob's age is its file's modification time, which is when it was
// downloaded. That is only as right as the clock was: a file written before
// the clock was set looks ancient and goes first. Nothing is evicted while
// the clock is not set.
//
// 1.x walked three stores once per age step (30 days, 15, ... 4 hours), up
// to eight walks, and knew nothing of pins: pinned artworks lived in a
// fourth store, as copies.
//
// p3a_evict.c is pure (host-tested); p3a_blob_evict.c walks the card.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_EVICT_MIN_AGE_S     (4 * 3600)
#define P3A_EVICT_MIN_CARD      (1024ull * 1024 * 1024)
#define P3A_EVICT_MAX_ENTRIES   32768  // blobs looked at in one run: 1 MiB of PSRAM for its length

// The marks for a card of `total_bytes`. False (and both 0) for a card too
// small to evict from.
bool p3a_evict_marks(uint64_t total_bytes, uint64_t *start_bytes, uint64_t *stop_bytes);

typedef struct {
    uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
    uint32_t modified_at;  // Unix seconds
    uint32_t size;
    uint8_t format;        // p3a_format_t
    uint8_t keep;          // the caller wants it kept
} p3a_evict_entry_t;

// Orders `entries` oldest first and says how many, from the front, to
// delete so that at least `need_bytes` are freed. Kept entries and entries
// younger than `min_age_s` at `now` are moved behind the others and never
// counted. `freed`, if given, receives the bytes those deletions free: less
// than `need_bytes` when there was not enough to delete.
uint32_t p3a_evict_choose(p3a_evict_entry_t *entries, uint32_t count, uint64_t need_bytes, uint32_t now,
                          uint32_t min_age_s, uint64_t *freed);

// Reads a blob's file name ("32 hex digits" + a known extension) into its
// reference and format. False for any other name: such a file is not a
// blob and is left alone.
bool p3a_evict_parse_name(const char *name, uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t *format);

// --- on the card (p3a_blob_evict.c) ---------------------------------------------

typedef bool (*p3a_blob_keep_fn)(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], void *ctx);

typedef struct {
    uint32_t seen;     // blobs in the store
    uint32_t deleted;
    uint64_t freed_bytes;
} p3a_blob_evict_t;

// Deletes the oldest blobs until `need_bytes` are freed, sparing those
// younger than `min_age_s` and those `keep` (may be NULL) answers true for.
// Walks the whole store: seconds on a full card, so call it from a task
// that may be slow.
//   ESP_ERR_INVALID_STATE  the clock is not set: ages mean nothing
//   P3A_ERR_SD_FAILED      the card has failed or is shared with a computer
//   ESP_ERR_NO_MEM
esp_err_t p3a_blob_evict(uint64_t need_bytes, uint32_t min_age_s, p3a_blob_keep_fn keep, void *ctx,
                         p3a_blob_evict_t *out);

#ifdef __cplusplus
}
#endif
