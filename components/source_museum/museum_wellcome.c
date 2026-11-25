// SPDX-License-Identifier: Apache-2.0
// Wellcome Collection, London. Pure.
//
//   listing  GET https://api.wellcomecollection.org/catalogue/v2/works
//                ?page=N&pageSize=100&items.locations.locationType=iiif-image
//                &include=items&{filter}={term}
//   axes     workType (the term is its id, "k" for pictures), and genres,
//            subjects, contributors (the term is the label itself: Wellcome
//            gives those no short id)
//   key      the image's id: the part of an item's iiif-image location
//            address after "/image/"
//   picture  https://iiif.wellcomecollection.org/image/{key}/full/!720,720/0/default.jpg
//
// No API key. The listing does not say how large a picture is.

#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 100
#define MARKER     "/image/"

static const char *filter_of(const char *axis)
{
    if (strcmp(axis, "workType") == 0) {
        return "workType";
    }
    if (strcmp(axis, "genres") == 0) {
        return "genres.label";
    }
    if (strcmp(axis, "subjects") == 0) {
        return "subjects.label";
    }
    if (strcmp(axis, "contributors") == 0) {
        return "contributors.agent.label";
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
    return museum_put(out, len, &at, "https://api.wellcomecollection.org/catalogue/v2/works?page=") &&
                   museum_put_number(out, len, &at, q->start / PAGE_ITEMS + 1) &&
                   museum_put(out, len, &at, "&pageSize=") && museum_put_number(out, len, &at, PAGE_ITEMS) &&
                   museum_put(out, len, &at, "&items.locations.locationType=iiif-image&include=items&") &&
                   museum_put(out, len, &at, filter) && museum_put(out, len, &at, "=") &&
                   museum_put_encoded(out, len, &at, q->term)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

// The image id in a work: its first item location of the iiif-image kind.
static bool image_id(const cJSON *work, char *out, size_t len)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(work, "items")) {
        const cJSON *location;
        cJSON_ArrayForEach(location, cJSON_GetObjectItemCaseSensitive(item, "locations")) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(location, "locationType"), "id");
            const cJSON *url = cJSON_GetObjectItemCaseSensitive(location, "url");
            if (!cJSON_IsString(type) || strcmp(type->valuestring, "iiif-image") != 0 || !cJSON_IsString(url)) {
                continue;
            }
            const char *start = strstr(url->valuestring, MARKER);
            if (start == NULL) {
                continue;
            }
            start += strlen(MARKER);
            const size_t n = strcspn(start, "/?#");
            if (n == 0 || n >= len) {
                continue;
            }
            memcpy(out, start, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

static esp_err_t parse_page(const char *json, museum_page_t *page)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *work;
    cJSON_ArrayForEach(work, results) {
        char key[P3A_RECORD_IIIF_KEY_LEN];
        page->items++;
        if (image_id(work, key, sizeof(key))) {
            museum_page_add(page, MUSEUM_WELLCOME, key, 0, 0);
        }
    }
    // Wellcome names the next page when there is one.
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "nextPage");
    page->more = page->items > 0 && cJSON_IsString(next) && next->valuestring[0] != 0;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    return museum_iiif_url("https://iiif.wellcomecollection.org/image", record, out, len);
}

const museum_t museum_wellcome = {
    .id = "wellcome",
    .number = MUSEUM_WELLCOME,
    .name = "Wellcome Collection",
    .page_items = PAGE_ITEMS,
    .offset_cap = 9900,  // the catalogue serves no page past 10,000 works
    .response_bytes = 384 * 1024,
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
