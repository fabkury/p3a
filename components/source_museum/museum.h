// SPDX-License-Identifier: Apache-2.0
// The museums: what p3a must know about each one to list a channel of its
// artworks and to fetch a picture. Pure (cJSON and the record only):
// host-tested on responses recorded from each museum
// (tests/host/sources/test_museums.c). Private to the museum source.
//
// A museum is a row in a table (museum_util.c) with three functions of its
// own, each in the museum's file (museum_vam.c, ...):
//
//   list_url     the address of one page of a channel's listing
//   parse_page   that page, read into records
//   image_url    where a record's picture is, at most 720 pixels a side
//
// Everything else is shared and lives in source_museum.c: the loop over
// pages, the key, the waits. 1.x had that loop nine times, once per museum,
// each copy 250 lines.
//
// A channel is "{museum}:{axis}" with a term: "vam:category" and
// "THES48903". An axis is one of the museum's ways of dividing its
// collection (a department, a category, a set).
//
// A record's key (identity.museum.iiif_key) is whatever the museum's
// picture address is built from; what that is, is the museum's business.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSEUM_LONGEST_SIDE 720  // of the picture asked for: the panel's size
#define MUSEUM_NEXT_LEN     384

// identity.museum.museum_id. Written to disk: never renumber. (1.x's numbers.)
typedef enum {
    MUSEUM_ARTIC = 0,
    MUSEUM_RIJKS = 1,
    MUSEUM_VAM = 2,
    MUSEUM_WELLCOME = 3,
    MUSEUM_SMK = 4,
    MUSEUM_HAM = 5,
    MUSEUM_SI = 6,
    MUSEUM_CMA = 7,
    MUSEUM_MIA = 8,
    MUSEUM_COUNT_
} museum_number_t;

typedef enum { MUSEUM_KEY_NONE = 0, MUSEUM_KEY_HARVARD, MUSEUM_KEY_SMITHSONIAN } museum_key_t;

// One page of a listing, asked for.
typedef struct {
    const char *axis;
    const char *term;
    const char *api_key;  // "" when the museum needs none
    uint32_t start;       // artworks before this page
    const char *next;     // what the page before said comes next; "" on the first page
} museum_query_t;

// One page of a listing, read.
typedef struct {
    p3a_record_t *out;  // in: where the records go
    uint32_t room;      // in: how many fit
    uint32_t now;       // in: Unix seconds for the records' order; 0 if the clock is not set
    uint32_t count;     // records written
    uint32_t items;     // items the page held, usable or not
    bool more;          // the museum has a page after this one
    char next[MUSEUM_NEXT_LEN];  // for a museum that pages by token: the next page's
} museum_page_t;

typedef struct {
    const char *id;       // "vam": the first part of a channel's kind
    uint16_t number;      // museum_number_t
    const char *name;     // "Victoria and Albert Museum"
    uint16_t page_items;  // asked for per request
    uint32_t offset_cap;  // an offset wraps here; the museum serves no listing past it
    uint32_t response_bytes;  // room for one page
    museum_key_t key;
    const char *unavailable;  // why its channels cannot be played now, or NULL
    bool (*axis_ok)(const char *axis);
    // ESP_ERR_INVALID_SIZE when it does not fit. Holds the key, for a museum that has one.
    esp_err_t (*list_url)(const museum_query_t *query, char *out, size_t len);
    // P3A_ERR_CORRUPT when the answer is not a listing.
    esp_err_t (*parse_page)(const char *json, museum_page_t *page);
    esp_err_t (*image_url)(const p3a_record_t *record, char *out, size_t len);
} museum_t;

// The museum of a channel's kind ("vam:category"), with the axis; NULL for
// a museum this firmware does not have or an axis the museum does not.
const museum_t *museum_of_kind(const char *kind, const char **axis);

// The museum a record came from; NULL for a number this firmware does not know.
const museum_t *museum_of_record(const p3a_record_t *record);

const museum_t *museum_at(size_t index);  // NULL past the end

// --- shared by the museums' files (museum_util.c) ---------------------------------

// Appends to `out` at `*at`; false when it does not fit.
bool museum_put(char *out, size_t len, size_t *at, const char *text);
// The same, percent-encoded (all but RFC 3986's unreserved characters).
bool museum_put_encoded(char *out, size_t len, size_t *at, const char *text);
bool museum_put_number(char *out, size_t len, size_t *at, uint32_t n);

// A key goes into a picture's address as it is: only characters that are
// safe there (letters, digits, - . _ ~), and not too long for a record.
bool museum_key_ok(const char *key);

// Adds a record for `key` to the page, if it is usable and there is room.
// `width` and `height` are the original's size when the listing says (0 if
// not): the record gets the size of the picture that will be fetched.
void museum_page_add(museum_page_t *page, uint16_t museum, const char *key, uint32_t width, uint32_t height);

// "{base}/{key}/full/!720,720/0/default.jpg": the IIIF address most of them share.
esp_err_t museum_iiif_url(const char *base, const p3a_record_t *record, char *out, size_t len);

// The museums.
extern const museum_t museum_vam, museum_smk;

#ifdef __cplusplus
}
#endif
