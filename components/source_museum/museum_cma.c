// SPDX-License-Identifier: Apache-2.0
// Cleveland Museum of Art. Pure.
//
//   listing  GET https://openaccess-api.clevelandart.org/api/artworks/
//                ?cc0=1&has_image=1&skip=N&limit=100&{axis}={term}
//                &fields=accession_number,images
//   axes     department, type (the term is the full name, matched exactly)
//   key      the accession number ("1926.1976")
//   picture  https://openaccess-cdn.clevelandart.org/{key}/{key}_web.jpg
//
// No API key, and no IIIF: every artwork has one "web" picture of a fixed
// size, 750 to 1300 pixels on the longer side, larger than the panel needs.
// cc0=1 keeps the listing to works in the public domain. The listing gives
// the web picture's size, as text and now and then empty.
//
// 1.x cut department names to 32 characters and kept a table to expand
// them again; a term is 95 characters in 2.0 and the names fit.

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 100

static bool axis_ok(const char *axis)
{
    return strcmp(axis, "department") == 0 || strcmp(axis, "type") == 0;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    if (!axis_ok(q->axis) || q->term == NULL || q->term[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://openaccess-api.clevelandart.org/api/artworks/?cc0=1&has_image=1&skip=") &&
                   museum_put_number(out, len, &at, q->start) && museum_put(out, len, &at, "&limit=") &&
                   museum_put_number(out, len, &at, PAGE_ITEMS) && museum_put(out, len, &at, "&") &&
                   museum_put(out, len, &at, q->axis) && museum_put(out, len, &at, "=") &&
                   museum_put_encoded(out, len, &at, q->term) &&
                   museum_put(out, len, &at, "&fields=accession_number,images")
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

// A size as Cleveland sends it: text, sometimes empty.
static uint32_t size_of(const cJSON *value)
{
    long n = 0;
    if (cJSON_IsString(value)) {
        n = strtol(value->valuestring, NULL, 10);
    } else if (cJSON_IsNumber(value)) {
        n = (long)value->valuedouble;
    }
    return n > 0 && n <= 100000 ? (uint32_t)n : 0;
}

static esp_err_t parse_page(const char *json, museum_page_t *page)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *artwork;
    cJSON_ArrayForEach(artwork, data) {
        page->items++;
        const cJSON *accession = cJSON_GetObjectItemCaseSensitive(artwork, "accession_number");
        const cJSON *web = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(artwork, "images"), "web");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(web, "url");
        // Without a web picture the address made from the number would be a 404.
        if (cJSON_IsString(accession) && cJSON_IsString(url) && url->valuestring[0] != 0) {
            museum_page_add(page, MUSEUM_CMA, accession->valuestring,
                            size_of(cJSON_GetObjectItemCaseSensitive(web, "width")),
                            size_of(cJSON_GetObjectItemCaseSensitive(web, "height")));
        }
    }
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(info, "total");
    const cJSON *skip = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(info, "parameters"), "skip");
    page->more = page->items > 0 && cJSON_IsNumber(total) && cJSON_IsNumber(skip) &&
                 skip->valuedouble + page->items < total->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    const char *key = record->identity.museum.iiif_key;
    if (len == 0 || !museum_key_ok(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://openaccess-cdn.clevelandart.org/") && museum_put(out, len, &at, key) &&
                   museum_put(out, len, &at, "/") && museum_put(out, len, &at, key) &&
                   museum_put(out, len, &at, "_web.jpg")
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

const museum_t museum_cma = {
    .id = "cma",
    .number = MUSEUM_CMA,
    .name = "Cleveland Museum of Art",
    .page_items = PAGE_ITEMS,
    .offset_cap = 40000,  // about as many public-domain works as it has; skip works at any depth
    .response_bytes = 256 * 1024,
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
