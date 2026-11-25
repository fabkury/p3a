// SPDX-License-Identifier: Apache-2.0
// Klipy's listings, read into records, and the addresses p3a asks Klipy
// for. Pure (cJSON and the record only): host-tested on responses recorded
// from the service (tests/host/sources/test_klipy_parse.c). Private to the
// Klipy source.
//
// How Klipy differs from Giphy:
//   - The API key is part of the address's path, not a parameter.
//   - A key Klipy does not know is answered 404, on a route that exists.
//   - Listings are paged by number, from 1.
//   - An item's files have addresses that cannot be worked out from its id:
//     random tokens, one per size and format. So a record holds only the
//     item's number, and the address is asked for when it is downloaded
//     (items?ids=).
//   - An item comes in four sizes (hd, md, sm, xs), each as GIF and WebP,
//     and the sizes are not an ordered ladder: md can be larger than hd.
//     p3a takes the largest that is no more than one and a half screens
//     each way, else the largest there is.
//   - Items carry no date.
//   - A listing can hold advertisements (type "ad"), which are skipped.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KLIPY_PAGE_ITEMS 50
#define KLIPY_OFFSET_CAP 2500  // an offset wraps here; trending reaches at least this far

// identity.klipy.product. Written to disk.
#define KLIPY_PRODUCT_GIF     0
#define KLIPY_PRODUCT_STICKER 1

typedef enum { KLIPY_MODE_TRENDING, KLIPY_MODE_SEARCH } klipy_mode_t;

// Reads a channel's kind: "{product}:{mode}", product "gif" or "sticker",
// mode "trending", "search" or "category" (a category is a search for its
// name). A kind with no product is a GIF one. False for anything else.
bool klipy_kind_parse(const char *kind, uint8_t *product, klipy_mode_t *mode);

typedef struct {
    uint8_t product;
    bool webp;               // the preferred format; the other is taken when it is all there is
    uint16_t screen_w, screen_h;
    uint32_t now;            // Unix seconds, for the records' order; 0 if the clock is not set
} klipy_parse_opts_t;

// Reads one page of a listing. As giphy_parse_page(); `*has_next` receives
// what the page says about a next one (when it does not say, whether it was
// full).
esp_err_t klipy_parse_page(const char *json, const klipy_parse_opts_t *opts, p3a_record_t *out, uint32_t room,
                           uint32_t *count, uint32_t *items, bool *has_next);

// Reads the answer to items?ids= and gives the address of the file to
// download.
//   P3A_ERR_MISSING   Klipy no longer has the item
//   P3A_ERR_CORRUPT   not an answer of that kind, or the item has no usable file
esp_err_t klipy_parse_item_url(const char *json, const klipy_parse_opts_t *opts, char *out, size_t len);

// The address of one page of a listing (page from 1). `query` NULL or ""
// for trending. It holds the API key: never log it.
esp_err_t klipy_list_url(const char *api_key, uint8_t product, const char *query, uint32_t page, const char *rating,
                         char *out, size_t len);

// The address that says where an item's files are. Holds the key too.
esp_err_t klipy_item_url(const char *api_key, const p3a_record_t *record, char *out, size_t len);

#ifdef __cplusplus
}
#endif
