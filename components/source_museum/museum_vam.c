// SPDX-License-Identifier: Apache-2.0
// Victoria and Albert Museum, London. Pure.
//
//   listing  GET https://api.vam.ac.uk/v2/objects/search
//                ?page=N&page_size=100&images_exist=1&id_{axis}={term}
//   axes     collection, category, venue
//   key      the record's _primaryImageId, given in the listing itself
//   picture  https://framemark.vam.ac.uk/collections/{key}/full/!720,720/0/default.jpg
//
// No API key. The listing does not say how large a picture is.

#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 100

static const char *filter_of(const char *axis)
{
    if (strcmp(axis, "collection") == 0) {
        return "id_collection";
    }
    if (strcmp(axis, "category") == 0) {
        return "id_category";
    }
    if (strcmp(axis, "venue") == 0) {
        return "id_venue";
    }
    return NULL;
}

static bool axis_ok(const char *axis)
{
    return filter_of(axis) != NULL;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    const char *filter = filter_of(q->axis);
    if (filter == NULL || q->term == NULL || q->term[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://api.vam.ac.uk/v2/objects/search?page=") &&
                   museum_put_number(out, len, &at, q->start / PAGE_ITEMS + 1) &&
                   museum_put(out, len, &at, "&page_size=") && museum_put_number(out, len, &at, PAGE_ITEMS) &&
                   museum_put(out, len, &at, "&images_exist=1&") && museum_put(out, len, &at, filter) &&
                   museum_put(out, len, &at, "=") && museum_put_encoded(out, len, &at, q->term)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t parse_page(const char *json, museum_page_t *page)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *records = cJSON_GetObjectItemCaseSensitive(root, "records");
    if (!cJSON_IsArray(records)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *record;
    cJSON_ArrayForEach(record, records) {
        page->items++;
        const cJSON *image = cJSON_GetObjectItemCaseSensitive(record, "_primaryImageId");
        if (cJSON_IsString(image)) {
            museum_page_add(page, MUSEUM_VAM, image->valuestring, 0, 0);
        }
    }
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(info, "page");
    const cJSON *of = cJSON_GetObjectItemCaseSensitive(info, "pages");
    page->more = page->items > 0 && cJSON_IsNumber(at) && cJSON_IsNumber(of) && at->valuedouble < of->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    return museum_iiif_url("https://framemark.vam.ac.uk/collections", record, out, len);
}

const museum_t museum_vam = {
    .id = "vam",
    .number = MUSEUM_VAM,
    .name = "Victoria and Albert Museum",
    .page_items = PAGE_ITEMS,
    .offset_cap = 9900,  // the search serves no page past 10,000 records
    .response_bytes = 256 * 1024,
    .axes = "collection category venue",
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
