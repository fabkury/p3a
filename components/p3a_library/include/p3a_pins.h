// SPDX-License-Identifier: Apache-2.0
// Pin lists: the artworks a person chose to keep.
//
// A pin is a record in a list, and the record names its bytes in the blob
// store. Nothing is copied: 1.x copied every pinned artwork into the list's
// own folder, in a sixth store with its own path builder. What a pin does
// to the bytes is keep them: eviction never deletes a blob that any list
// names (p3a_pins_keeps).
//
// A list is an index file like any channel's, flagged as the user's data:
//
//   {root}/pins/lists.tbl      the lists: slug, name, which one is active
//   {root}/pins/{slug}.idx     one list's records, in the order pinned
//
// Up to 64 lists of up to 4096 pins. One list is the active one: a swipe
// pins into it. There is always at least one list ("My Pins" is made when
// there is none), and the active list cannot be deleted. A user's own file
// cannot be pinned (it is not in the blob store, and it is already kept).
//
// A list plays as a channel: source "pinned", identifier the list's slug.
// A pinned record keeps its own source, so it is scaled as that source's
// artworks are, in a list as in its own channel.
//
// The table of lists and the set of kept blobs are pure
// (p3a_pin_table.c, host-tested); p3a_pins.c is the files and the lock.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_index.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_PINS_MAX_LISTS   64
#define P3A_PIN_LIST_MAX     4096
#define P3A_PIN_SLUG_LEN     9   // 8 hex digits and the terminator
#define P3A_PIN_NAME_LEN     65  // 64 bytes of UTF-8 and the terminator

// --- the table of lists (pure) ----------------------------------------------------

// One list, as stored. 80 bytes on disk.
typedef struct __attribute__((packed)) {
    char slug[P3A_PIN_SLUG_LEN];
    char name[P3A_PIN_NAME_LEN];
    uint8_t active;       // 1 for the active list
    uint8_t reserved[5];  // zero
} p3a_pin_list_t;
_Static_assert(sizeof(p3a_pin_list_t) == 80, "a pin list entry is 80 bytes on disk");

#define P3A_PIN_TABLE_VERSION 1

typedef struct {
    p3a_pin_list_t lists[P3A_PINS_MAX_LISTS];
    uint32_t count;
} p3a_pin_table_t;

// A name a list may have: 1 to 64 bytes, no control characters.
bool p3a_pin_name_ok(const char *name);
// A slug as the table makes them: 8 lower-case hex digits.
bool p3a_pin_slug_ok(const char *slug);

void p3a_pin_table_init(p3a_pin_table_t *table);

// Takes the lists read from a file. Every entry is checked (slug, name,
// zero padding, no slug twice); one bad entry refuses the lot
// (P3A_ERR_CORRUPT) and leaves the table empty. If no entry or more than
// one is marked active, the first is.
esp_err_t p3a_pin_table_set(p3a_pin_table_t *table, const p3a_pin_list_t *lists, uint32_t count);

// Adds a list. The slug is made from the name and `salt`, and changed until
// no other list has it. ESP_ERR_INVALID_ARG for a name that will not do,
// ESP_ERR_NO_MEM when there are 64 lists. The first list made is active.
esp_err_t p3a_pin_table_create(p3a_pin_table_t *table, const char *name, uint32_t salt, char slug[P3A_PIN_SLUG_LEN]);

// P3A_ERR_MISSING for a slug that names no list.
esp_err_t p3a_pin_table_rename(p3a_pin_table_t *table, const char *slug, const char *name);
esp_err_t p3a_pin_table_set_active(p3a_pin_table_t *table, const char *slug);
// ESP_ERR_INVALID_STATE for the active list.
esp_err_t p3a_pin_table_delete(p3a_pin_table_t *table, const char *slug);

const p3a_pin_list_t *p3a_pin_table_find(const p3a_pin_table_t *table, const char *slug);
const p3a_pin_list_t *p3a_pin_table_active(const p3a_pin_table_t *table);  // NULL for an empty table

// --- the kept blobs (pure) ----------------------------------------------------------
//
// Every blob reference any list names, sorted, each as many times as it is
// pinned (the same artwork may be in two lists).

typedef struct {
    uint8_t (*refs)[P3A_RECORD_BLOB_REF_LEN];
    uint32_t count;
    uint32_t capacity;
} p3a_pin_keep_t;

// ESP_ERR_NO_MEM when it cannot grow. An all-zero reference is not kept.
esp_err_t p3a_pin_keep_add(p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN]);
// Takes one pinning of `ref` away; nothing if it is not there.
void p3a_pin_keep_remove(p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN]);
bool p3a_pin_keep_has(const p3a_pin_keep_t *keep, const uint8_t ref[P3A_RECORD_BLOB_REF_LEN]);
void p3a_pin_keep_free(p3a_pin_keep_t *keep);

// --- the lists on the card (p3a_pins.c) ----------------------------------------------

typedef struct {
    char slug[P3A_PIN_SLUG_LEN];
    char name[P3A_PIN_NAME_LEN];
    bool active;
    uint32_t count;  // pins in it
} p3a_pin_list_info_t;

// Reads the table and every list (for the kept blobs). Makes "My Pins" if
// there is no list. Call once at boot, after storage. A table or a list
// that cannot be read is left as it is on the card: the table then starts
// empty in memory and nothing is written until a list is made.
esp_err_t p3a_pins_start(void);

// The lists, in the order they were made. `*count` receives how many.
esp_err_t p3a_pins_lists(p3a_pin_list_info_t *out, size_t capacity, size_t *count);

esp_err_t p3a_pins_create(const char *name, char slug[P3A_PIN_SLUG_LEN]);
esp_err_t p3a_pins_rename(const char *slug, const char *name);
esp_err_t p3a_pins_set_active(const char *slug);
// Deletes a list and its pins. ESP_ERR_INVALID_STATE for the active list.
esp_err_t p3a_pins_delete(const char *slug);

// Pins an artwork into a list (`slug` NULL: the active one).
//   ESP_ERR_NOT_SUPPORTED   it cannot be pinned: a user's own file, or its bytes are not in the store
//   ESP_ERR_INVALID_STATE   it is in the list already
//   ESP_ERR_NO_MEM          the list is full
//   P3A_ERR_MISSING         no such list
esp_err_t p3a_pins_pin(const char *slug, const p3a_record_t *record);
// ESP_ERR_NOT_FOUND when it is not in the list, P3A_ERR_MISSING when there is no such list.
esp_err_t p3a_pins_unpin(const char *slug, const p3a_record_t *record);
bool p3a_pins_is_pinned(const char *slug, const p3a_record_t *record);

// A list's records, in the order pinned, into `index` (created by the
// caller with room for P3A_PIN_LIST_MAX).
esp_err_t p3a_pins_load(const char *slug, p3a_index_t *index);

// Whether any list names this blob: eviction then leaves it alone. Cheap
// (a search in memory), and callable from any task.
bool p3a_pins_keeps(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN]);

// How many times the lists have changed since boot: a pin, an unpin, a
// list made, renamed, deleted or made active.
uint32_t p3a_pins_revision(void);

#ifdef __cplusplus
}
#endif
