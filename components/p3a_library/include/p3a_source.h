// SPDX-License-Identifier: Apache-2.0
// The source interface: what every place artworks come from must be able to
// do, as a table of functions. A source registers its table at boot; the
// scheduler, the download manager and the API call through it and never name
// a source.
//
// 1.x had no such table: each source was an arm in about a dozen type
// ladders (63 comparisons in the refresh code alone), and each new source
// added an arm to every one.
//
// The interface lives in the library layer and the sources sit above it:
// they include this header and the record's, and nothing below includes
// theirs. Pure (host-tested).
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

// A channel's source number: p3a_source_t for the five places artworks come
// from, and one more for pinned lists, whose records keep their own source.
// The same numbers as playset_source_t; written to disk.
#define P3A_CHANNEL_SOURCE_PINNED 6
#define P3A_CHANNEL_SOURCE_END_   7

// Which of a source's channels: the three fields of a playset channel that,
// with the source, make its id.
typedef struct {
    const char *kind;
    const char *identifier;
    uint32_t offset;
} p3a_channel_ref_t;

typedef struct {
    uint8_t source;    // the channel source number this table serves
    const char *name;  // "local", "giphy", ...

    // Lists the channel into `index`, replacing or merging as the source
    // sees fit, and leaves each record's availability right. May take long
    // (a card scan, network requests): call it from a job, never from a task
    // that must stay responsive. P3A_ERR_NO_KEY, P3A_ERR_RATE_LIMITED,
    // P3A_ERR_AUTH and P3A_ERR_SD_FAILED mean what p3a_err.h says.
    esp_err_t (*refresh)(const p3a_channel_ref_t *channel, p3a_index_t *index);

    // Where a record's bytes are when they are not in the blob store (records
    // flagged P3A_RECORD_IN_PLACE). NULL for a source whose artworks are
    // always blobs.
    esp_err_t (*path)(const p3a_record_t *record, char *out, size_t len);

    // How often a channel should be listed again, in seconds; 0 for a source
    // that is listed only when something says it changed.
    uint32_t (*refresh_interval_s)(void);

    // Whether this source's pictures are scaled smoothly (photographs,
    // paintings) or with sharp pixels (pixel art).
    bool smooth;
} p3a_source_ops_t;

// Registers a source. The table must stay valid for good (make it static).
// ESP_ERR_INVALID_ARG for a missing name or refresh function or a number out
// of range; ESP_ERR_INVALID_STATE when that number is already taken.
esp_err_t p3a_source_register(const p3a_source_ops_t *ops);

// The table of a channel source, or NULL when none is registered: a playset
// may name a source this build does not have, and its channel is then
// simply never chosen.
const p3a_source_ops_t *p3a_source_get(uint8_t source);

// For tests: forgets every registration.
void p3a_source_reset_for_test(void);

#ifdef __cplusplus
}
#endif
