// SPDX-License-Identifier: Apache-2.0
// The playset: what the device is asked to play. A named list of channels,
// each with a weight. One form in memory, on the SD card and (as JSON) over
// the API.
//
// 1.x had three forms: JSON, a packed 144-byte entry on disk, and an 860-byte
// in-memory struct that was also dumped raw into a file. Identifiers were 32
// characters, which forced two shipped workarounds (CMA, Wellcome). Here the
// packed entry is the only form, identifiers are 95 characters, and anything
// that does not fit is refused, never cut.
//
// How to play (pick mode, dwell time) is not in a playset: those are the
// user's settings and hold across playsets.
//
// Two things a channel may set for itself, in this playset only (D100): how
// many artworks it holds (`cache_size`) and how often it is listed again
// (`refresh_s`). Zero, the usual, means what its source's settings say. The
// same channel may ask differently in two playsets: its index on the card
// is one, kept at the deepest size asked (p3a_index.h). What a source allows
// is the range of its own two settings (p3a_source_ops_t.channel_range);
// the API refuses a value outside it, and the scheduler holds any other
// within it (playset_value).
//
// playset.c is pure (cJSON only; host-tested). playset_store.c keeps playsets
// on the card.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYSET_MAX_CHANNELS 64
#define PLAYSET_NAME_LEN     33   // sizes include the terminator
#define PLAYSET_KIND_LEN     32
#define PLAYSET_IDENT_LEN    96
#define PLAYSET_DISPLAY_LEN  64
#define PLAYSET_MAX_WEIGHT   1000000u
#define PLAYSET_MAX_CACHE_SIZE 4096u  // the largest index there is (P3A_INDEX_MAX_RECORDS)
#define PLAYSET_VERSION      1    // of the packed layout, as written in a container's header
#define PLAYSET_CHANNEL_ID_LEN 17 // 16 hex digits and the terminator

// Which source a channel belongs to. Written to disk: never renumber, only
// append. 1 to 5 are the same numbers as p3a_source_t; a pinned list is a
// channel source of its own although its records keep theirs.
typedef enum {
    PLAYSET_SOURCE_LOCAL = 1,
    PLAYSET_SOURCE_MAKAPIX = 2,
    PLAYSET_SOURCE_GIPHY = 3,
    PLAYSET_SOURCE_KLIPY = 4,
    PLAYSET_SOURCE_MUSEUM = 5,
    PLAYSET_SOURCE_PINNED = 6,
    PLAYSET_SOURCE_TEST = 7,  // the test hooks' source; a release build has none and never plays its channels
    PLAYSET_SOURCE_END_
} playset_source_t;

// One channel of a playset. 224 bytes, the same in memory and on disk.
typedef struct __attribute__((packed)) {
    uint8_t source;       // playset_source_t
    uint8_t reserved0[3]; // zero
    uint32_t weight;      // relative to the other channels; all zero means equal shares
    uint32_t offset;      // where in the source's listing this channel starts
    char kind[PLAYSET_KIND_LEN];            // which of the source's channels: "all", "trending", "search", ...
    char identifier[PLAYSET_IDENT_LEN];     // its parameter: a user, a tag, a query, a museum term
    char display_name[PLAYSET_DISPLAY_LEN]; // what to call it on screen; may be empty
    uint32_t refresh_s;   // seconds between two listings of this channel here; 0: its source's setting
    uint16_t cache_size;  // artworks this channel holds here; 0: its source's setting
    uint8_t reserved[14]; // zero
} playset_channel_t;

_Static_assert(sizeof(playset_channel_t) == 224, "a playset channel is 224 bytes on disk");
// The two fields took six of the bytes that were reserved, and zero means
// what it meant: the layout's version did not change.

typedef struct {
    char name[PLAYSET_NAME_LEN];  // empty for a playset that was never saved under a name
    uint32_t count;               // 1 to PLAYSET_MAX_CHANNELS
    playset_channel_t channels[PLAYSET_MAX_CHANNELS];
} playset_t;  // about 14 KB: allocate it, do not put it on a task's stack

// Checks a playset whatever it was built from. On failure `why` (if given)
// says what is wrong in words fit for an API answer.
//   ESP_ERR_INVALID_ARG  a count, name, source or weight out of range
//   P3A_ERR_CORRUPT      text not terminated in its field, or reserved bytes set:
//                        cannot come from this code, only from a damaged file
esp_err_t playset_check(const playset_t *playset, char *why, size_t why_len);

// Builds a playset from its JSON form:
//   {"name": "...", "channels": [{"source": "giphy", "kind": "search", "identifier": "cats",
//                                  "display_name": "Cats", "weight": 2, "offset": 0,
//                                  "cache_size": 0, "refresh_s": 0}, ...]}
// Only "source" is required in a channel. Unknown keys, wrong types and text
// that does not fit are refused (ESP_ERR_INVALID_ARG) with the reason in `why`.
esp_err_t playset_from_json(const cJSON *json, playset_t *out, char *why, size_t why_len);

// The JSON form, with each channel's id added as "id". NULL when out of memory.
cJSON *playset_to_json(const playset_t *playset);

// What a channel gets for one of its two own values: `own` if it set one
// and its source allows any (`max` above zero), held within `min` to `max`;
// otherwise `standard`, the source's.
uint32_t playset_value(uint32_t own, uint32_t standard, uint32_t min, uint32_t max);

// A channel's id: 16 hex digits over its source, kind, identifier and offset.
// Two channels with the same four are the same channel (and share one index
// on the card) whatever they are called or weigh, and whatever size or
// refresh interval they ask for.
void playset_channel_id(const playset_channel_t *channel, char out[PLAYSET_CHANNEL_ID_LEN]);

// The packed form for a container: one header record carrying the name,
// then one record per channel, all PLAYSET_RECORD_SIZE bytes.
#define PLAYSET_RECORD_SIZE 224
// Writes count + 1 records to `records` (room for PLAYSET_MAX_CHANNELS + 1).
uint32_t playset_pack(const playset_t *playset, void *records);
// Reads records that came from a file; everything is checked.
// P3A_ERR_CORRUPT or ESP_ERR_INVALID_ARG as for playset_check().
esp_err_t playset_unpack(const void *records, uint32_t record_count, playset_t *out);

// "local", "makapix", ...; NULL for a number that is not a source.
const char *playset_source_name(uint8_t source);
// 0 for a name that is not a source.
uint8_t playset_source_parse(const char *name);

#ifdef __cplusplus
}
#endif
