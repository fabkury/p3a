// SPDX-License-Identifier: Apache-2.0
// Art Institute of Chicago. Pure. Not playable at present: see below.
//
//   listing  GET https://api.artic.edu/api/v1/artworks/search
//                ?query[term][{field}]={term}&page=N&limit=100&fields=id,image_id
//   axes     departments, classifications, subjects, themes, galleries,
//            artwork-types (each is a field of the search: department_id, ...)
//   key      the record's image_id
//   picture  https://www.artic.edu/iiif/2/{key}/full/!720,720/0/default.jpg
//
// No API key. Since August 2026 the museum's image server answers every
// client that is not a web browser with a challenge page (403), whatever
// the client says it is; the listing still works. So the museum is in the
// table with `unavailable` set: its channels report that they cannot be
// played, and nothing is asked of it. To bring it back when the block is
// lifted (art-institute-of-chicago/data-aggregator#151), clear
// `unavailable` and check a picture from the device.
//
// The search serves the first 1,000 results of a query to anonymous
// callers. 1.x reached further with a second kind of query; that is not
// carried over while the museum cannot be played.

#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 100

static const char *field_of(const char *axis)
{
    static const struct {
        const char *axis, *field;
    } map[] = {{"departments", "department_id"},  {"classifications", "classification_id"},
               {"subjects", "subject_id"},        {"themes", "category_ids"},
               {"galleries", "gallery_id"},       {"artwork-types", "artwork_type_id"}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(axis, map[i].axis) == 0) {
            return map[i].field;
        }
    }
    return NULL;
}

static bool axis_ok(const char *axis)
{
    return field_of(axis) != NULL;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    const char *field = field_of(q->axis);
    if (field == NULL || q->term == NULL || q->term[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    // "query[term][field]" with its brackets percent-encoded.
    return museum_put(out, len, &at, "https://api.artic.edu/api/v1/artworks/search?query%5Bterm%5D%5B") &&
                   museum_put(out, len, &at, field) && museum_put(out, len, &at, "%5D=") &&
                   museum_put_encoded(out, len, &at, q->term) && museum_put(out, len, &at, "&page=") &&
                   museum_put_number(out, len, &at, q->start / PAGE_ITEMS + 1) && museum_put(out, len, &at, "&limit=") &&
                   museum_put_number(out, len, &at, PAGE_ITEMS) && museum_put(out, len, &at, "&fields=id,image_id")
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
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
        const cJSON *image = cJSON_GetObjectItemCaseSensitive(artwork, "image_id");
        if (cJSON_IsString(image)) {  // null for an artwork with no picture
            museum_page_add(page, MUSEUM_ARTIC, image->valuestring, 0, 0);
        }
    }
    const cJSON *pagination = cJSON_GetObjectItemCaseSensitive(root, "pagination");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(pagination, "current_page");
    const cJSON *of = cJSON_GetObjectItemCaseSensitive(pagination, "total_pages");
    // Ten pages of a hundred are all an anonymous caller is served.
    page->more = page->items > 0 && cJSON_IsNumber(at) && cJSON_IsNumber(of) && at->valuedouble < of->valuedouble &&
                 at->valuedouble < 10;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    return museum_iiif_url("https://www.artic.edu/iiif/2", record, out, len);
}

const museum_t museum_artic = {
    .id = "artic",
    .number = MUSEUM_ARTIC,
    .name = "Art Institute of Chicago",
    .page_items = PAGE_ITEMS,
    .offset_cap = 900,
    .response_bytes = 128 * 1024,
    .unavailable = "its image server refuses clients that are not web browsers",
    .axes = "departments classifications subjects themes galleries artwork-types",
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
