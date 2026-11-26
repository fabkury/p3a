// SPDX-License-Identifier: Apache-2.0
// The museum source: channels of artworks from public museum collections.
//
//   kind "{museum}:{axis}", identifier "{term}"
//
// for example kind "vam:category", identifier "THES48903" (the Victoria
// and Albert Museum's prints). The museums and their axes are in the
// component's museum_*.c files. A channel's offset starts the listing that
// many artworks down.
//
// Pictures are fetched as JPEG, at most 720 pixels on the longer side.
// Harvard and the Smithsonian need the user's own API key (settings);
// without it their channels report P3A_ERR_NO_KEY.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot, before the scheduler starts.
esp_err_t source_museum_start(void);

// What the firmware knows about a museum, for whoever offers them to a
// person (the web page): the one table, so that no second list is kept.
typedef struct {
    const char *id;           // "vam"
    const char *name;         // "Victoria and Albert Museum"
    const char *axes;         // "collection category venue"
    bool needs_key;           // the user's own API key is needed to list its channels
    bool key_set;             // and one is saved
    const char *unavailable;  // why it cannot be played now, or NULL
} source_museum_info_t;

// The museum at `index` (from 0); false past the last.
bool source_museum_info(size_t index, source_museum_info_t *out);

// The id ("vam") of the museum a record names by number
// (identity.museum.museum_id), or NULL if this firmware has no such museum.
const char *source_museum_id(uint16_t number);

#ifdef __cplusplus
}
#endif
