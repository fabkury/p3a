// SPDX-License-Identifier: Apache-2.0
// Giphy's listing, read into records, and the addresses p3a asks Giphy
// for. Pure (cJSON and the record only): host-tested on responses recorded
// from the service (tests/host/sources/test_giphy_parse.c). Private to the
// Giphy source.
//
// Which file of a GIF p3a downloads is part of its record
// (identity.giphy.file), decided when the channel is listed:
//
//   - the rendition and format the settings ask for (fixed_height as GIF by
//     default: 200 pixels high), or
//   - with "prefer the downsized rendition" on, Giphy's downsized_medium
//     when the listing says it exists and it is no larger than one and a
//     half screens each way: a much better picture on a 720-pixel panel.
//
// Giphy makes a downsized-medium file only for some GIFs. For the rest the
// listing's downsized_medium points at the original, giphy.gif, and asking
// for the file by its guessed name is a 404. So the listing's own address
// is read, never guessed.
//
// Because the file is in the record, the download address needs nothing
// but the record, and changing the rendition setting makes the next listing
// name other files, which are then fetched.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GIPHY_PAGE_ITEMS  50   // asked for per request
#define GIPHY_OFFSET_CAP  499  // Giphy serves no listing past this offset

// identity.giphy.file. Written to disk: never renumber.
typedef enum {
    GIPHY_FILE_DEFAULT = 0,  // a record that names none: fixed_height as GIF
    GIPHY_FILE_200_GIF = 1,
    GIPHY_FILE_200_WEBP = 2,
    GIPHY_FILE_200W_GIF = 3,
    GIPHY_FILE_200W_WEBP = 4,
    GIPHY_FILE_ORIGINAL_GIF = 5,
    GIPHY_FILE_ORIGINAL_WEBP = 6,
    GIPHY_FILE_DOWNSIZED_MEDIUM = 7,
    GIPHY_FILE_DOWNSIZED = 8,
    GIPHY_FILE_END_
} giphy_file_t;

// The file a rendition setting ("fixed_height", "fixed_width", "original",
// "downsized_medium", "downsized") and format name. An unknown rendition is
// fixed_height; the downsized ones exist only as GIF.
giphy_file_t giphy_file_for(const char *rendition, bool webp);

// The name Giphy's listing gives the rendition of a file ("fixed_height",
// ...): what is asked for in `fields`.
const char *giphy_file_rendition(giphy_file_t file);

typedef struct {
    giphy_file_t file;       // the configured file
    bool prefer_downsized;
    uint16_t screen_w, screen_h;
    uint32_t now;            // Unix seconds, for an item with no date; 0 if the clock is not set
} giphy_parse_opts_t;

// Reads one page of a listing. Records are written to `out` (at most
// `room`); `*count` receives how many, `*items` how many items the page
// held (an item p3a cannot use is skipped, but counts here: it says whether
// there is a next page).
//   P3A_ERR_CORRUPT   not JSON, or no "data" array
esp_err_t giphy_parse_page(const char *json, const giphy_parse_opts_t *opts, p3a_record_t *out, uint32_t room,
                           uint32_t *count, uint32_t *items);

// Where a record's bytes are. ESP_ERR_INVALID_SIZE when it does not fit.
esp_err_t giphy_download_url(const p3a_record_t *record, char *out, size_t len);

typedef struct {
    const char *api_key;
    const char *query;      // NULL or "" for trending
    const char *rating;
    const char *country;    // "" for none
    const char *random_id;  // "" for none
    giphy_file_t file;
    bool prefer_downsized;
    uint32_t offset;
} giphy_list_request_t;

// The address of one page of a listing. ESP_ERR_INVALID_SIZE when it does
// not fit. It holds the API key: never log it.
esp_err_t giphy_list_url(const giphy_list_request_t *request, char *out, size_t len);

// The address that gives a device its random_id. Holds the key too.
esp_err_t giphy_random_id_url(const char *api_key, char *out, size_t len);

// Reads the random_id out of the answer to /v1/randomid.
esp_err_t giphy_parse_random_id(const char *json, char *out, size_t len);

// A click is told to Giphy in two requests: the first asks where to report
// it (this address, which holds the key), the second reports it there.
esp_err_t giphy_click_lookup_url(const char *api_key, const p3a_record_t *record, const char *random_id, char *out,
                                 size_t len);

// Reads the answer to the first, and makes the address of the second:
// Giphy's own, with the device's id and the time (Unix milliseconds) added.
//   P3A_ERR_CORRUPT   no such address in the answer, or not one at Giphy
esp_err_t giphy_parse_click_url(const char *json, const char *random_id, uint64_t now_ms, char *out, size_t len);

// "YYYY-MM-DD HH:MM:SS" (UTC) as Unix seconds; 0 for anything else, and for
// Giphy's "0000-00-00 00:00:00".
uint32_t giphy_parse_datetime(const char *text);

#ifdef __cplusplus
}
#endif
