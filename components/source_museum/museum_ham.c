// SPDX-License-Identifier: Apache-2.0
// Harvard Art Museums. Pure.
//
//   listing  GET https://api.harvardartmuseums.org/object
//                ?apikey={key}&size=100&page=N&hasimage=1&q=imagepermissionlevel:0
//                &{axis}={term}&sort=id&sortorder=asc&fields=id,primaryimageurl
//   axes     classification, century, culture, period, place, medium,
//            technique, worktype, group, gallery (the term is the id Harvard
//            gives it)
//   key      what follows "https://nrs.harvard.edu/" in primaryimageurl:
//            a name like "urn-3:HUAM:79762_dynmc"
//   picture  https://nrs.harvard.edu/{key}/full/!720,720/0/default.jpg,
//            which sends the request on to ids.lib.harvard.edu
//
// The user's own API key (2,500 requests a day). It is needed for listing
// only: pictures are fetched without it. A key Harvard does not know is a
// 401. Without imagepermissionlevel:0 about half the records come back with
// no picture address. The listing does not say how large a picture is.

#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 100
#define PREFIX     "https://nrs.harvard.edu/"

static bool axis_ok(const char *axis)
{
    static const char *const axes[] = {"classification", "century", "culture", "period", "place",
                                       "medium", "technique", "worktype", "group", "gallery"};
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
        if (strcmp(axis, axes[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    if (!axis_ok(q->axis) || q->term == NULL || q->term[0] == 0 || q->api_key == NULL || q->api_key[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://api.harvardartmuseums.org/object?apikey=") &&
                   museum_put_encoded(out, len, &at, q->api_key) && museum_put(out, len, &at, "&size=") &&
                   museum_put_number(out, len, &at, PAGE_ITEMS) && museum_put(out, len, &at, "&page=") &&
                   museum_put_number(out, len, &at, q->start / PAGE_ITEMS + 1) &&
                   museum_put(out, len, &at, "&hasimage=1&q=imagepermissionlevel:0&") &&
                   museum_put(out, len, &at, q->axis) && museum_put(out, len, &at, "=") &&
                   museum_put_encoded(out, len, &at, q->term) &&
                   museum_put(out, len, &at, "&sort=id&sortorder=asc&fields=id,primaryimageurl")
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
        const cJSON *image = cJSON_GetObjectItemCaseSensitive(record, "primaryimageurl");
        if (cJSON_IsString(image) && strncmp(image->valuestring, PREFIX, strlen(PREFIX)) == 0) {
            museum_page_add(page, MUSEUM_HAM, image->valuestring + strlen(PREFIX), 0, 0);
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
    return museum_iiif_url("https://nrs.harvard.edu", record, out, len);
}

const museum_t museum_ham = {
    .id = "ham",
    .number = MUSEUM_HAM,
    .name = "Harvard Art Museums",
    .page_items = PAGE_ITEMS,
    .offset_cap = 9900,
    .response_bytes = 128 * 1024,  // two fields per record: a page is small
    .key = MUSEUM_KEY_HARVARD,
    .axes = "classification century culture period place medium technique worktype group gallery",
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
