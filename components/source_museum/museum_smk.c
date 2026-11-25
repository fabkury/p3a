// SPDX-License-Identifier: Apache-2.0
// Statens Museum for Kunst, Copenhagen. Pure.
//
//   listing  GET https://api.smk.dk/api/v1/art/search
//                ?keys=*&offset=N&rows=50&filters=[collection:{term}],[has_image:true]
//            (the filters are one value, percent-encoded as a whole)
//   axes     collection
//   key      the JP2 file's name: what follows the last "/iiif/jp2/" in the
//            record's image_iiif_id
//   picture  https://iip.smk.dk/iiif/jp2/{key}/full/!720,720/0/default.jpg
//
// No API key. A record is about 4 KB and the API has no way to ask for
// less, so a page is 50 records in up to half a megabyte. The listing gives
// the original's size. The server offers WebP and answers 400 to a request
// for it: JPEG only.

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 50
#define MARKER     "/iiif/jp2/"

static bool axis_ok(const char *axis)
{
    return strcmp(axis, "collection") == 0;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    if (!axis_ok(q->axis) || q->term == NULL || q->term[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // A term is a collection's name and goes inside brackets: one that
    // holds a bracket or a comma would end the filter early.
    if (strpbrk(q->term, "[],") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char filters[256];
    const int n = snprintf(filters, sizeof(filters), "[collection:%s],[has_image:true]", q->term);
    if (n < 0 || (size_t)n >= sizeof(filters)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://api.smk.dk/api/v1/art/search?keys=*&offset=") &&
                   museum_put_number(out, len, &at, q->start) && museum_put(out, len, &at, "&rows=") &&
                   museum_put_number(out, len, &at, PAGE_ITEMS) && museum_put(out, len, &at, "&filters=") &&
                   museum_put_encoded(out, len, &at, filters)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static uint32_t number_of(const cJSON *value)
{
    return cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble < 4294967295.0
               ? (uint32_t)value->valuedouble
               : 0;
}

static esp_err_t parse_page(const char *json, museum_page_t *page)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        page->items++;
        const cJSON *iiif = cJSON_GetObjectItemCaseSensitive(item, "image_iiif_id");
        if (!cJSON_IsString(iiif)) {
            continue;  // some records have only a thumbnail
        }
        // After the last marker: it has been seen twice in one address.
        const char *key = NULL;
        for (const char *p = strstr(iiif->valuestring, MARKER); p != NULL; p = strstr(p + 1, MARKER)) {
            key = p + strlen(MARKER);
        }
        if (key != NULL) {
            museum_page_add(page, MUSEUM_SMK, key, number_of(cJSON_GetObjectItemCaseSensitive(item, "image_width")),
                            number_of(cJSON_GetObjectItemCaseSensitive(item, "image_height")));
        }
    }
    const uint32_t offset = number_of(cJSON_GetObjectItemCaseSensitive(root, "offset"));
    const uint32_t found = number_of(cJSON_GetObjectItemCaseSensitive(root, "found"));
    page->more = page->items > 0 && offset + page->items < found;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    return museum_iiif_url("https://iip.smk.dk/iiif/jp2", record, out, len);
}

const museum_t museum_smk = {
    .id = "smk",
    .number = MUSEUM_SMK,
    .name = "Statens Museum for Kunst",
    .page_items = PAGE_ITEMS,
    .offset_cap = 10000,
    .response_bytes = 512 * 1024,
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
