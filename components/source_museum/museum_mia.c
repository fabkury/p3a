// SPDX-License-Identifier: Apache-2.0
// Minneapolis Institute of Art. Pure.
//
//   listing  GET https://search.artsmia.org/{query}?size=50&from=N
//            where {query}, percent-encoded as one path segment, is
//            {axis}:"{term}" AND rights_type:"Public Domain" AND image:valid
//   axes     classification, department, country, style
//   key      the object's number ("529")
//   picture  https://1.api.artsmia.org/800/{key}.jpg
//
// No API key, and no IIIF: the search is an Elasticsearch index reached
// directly, and pictures exist at three fixed widths (400, 800, full); 800
// is fetched. A record is about 4 KB and cannot be trimmed, so a page is 50
// in up to half a megabyte.
//
// The index serves nothing past 10,000 results: there it answers a bare
// "[]" or a 500. The first is read as an empty page.
//
// About one in twenty records that say "image:valid" have no picture at any
// width and answer 403 to everyone (reported to the museum,
// artsmia/collection-elasticsearch#10). They are found out one by one when
// their download is refused.
//
// The listing gives the original scan's size, not the 800-pixel picture's:
// a record's size is right in shape, not in pixels, until it is decoded.

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 50

static bool axis_ok(const char *axis)
{
    return strcmp(axis, "classification") == 0 || strcmp(axis, "department") == 0 || strcmp(axis, "country") == 0 ||
           strcmp(axis, "style") == 0;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    if (!axis_ok(q->axis) || q->term == NULL || q->term[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // The term goes inside quotation marks in a query: one that holds a
    // mark or a backslash would end the phrase early.
    if (strpbrk(q->term, "\"\\") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char query[320];
    const int n = snprintf(query, sizeof(query), "%s:\"%s\" AND rights_type:\"Public Domain\" AND image:valid", q->axis,
                           q->term);
    if (n < 0 || (size_t)n >= sizeof(query)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://search.artsmia.org/") && museum_put_encoded(out, len, &at, query) &&
                   museum_put(out, len, &at, "?size=") && museum_put_number(out, len, &at, PAGE_ITEMS) &&
                   museum_put(out, len, &at, "&from=") && museum_put_number(out, len, &at, q->start)
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
    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) == 0) {
        cJSON_Delete(root);
        return ESP_OK;  // past the end of what the index serves: an empty last page
    }
    const cJSON *outer = cJSON_GetObjectItemCaseSensitive(root, "hits");
    const cJSON *hits = cJSON_GetObjectItemCaseSensitive(outer, "hits");
    if (!cJSON_IsArray(hits)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *hit;
    cJSON_ArrayForEach(hit, hits) {
        page->items++;
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(hit, "_source");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(source, "id");
        char key[16];
        if (cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble < 4000000000.0) {
            snprintf(key, sizeof(key), "%u", (unsigned)id->valuedouble);
        } else if (cJSON_IsString(id) && strlen(id->valuestring) < sizeof(key) &&
                   strspn(id->valuestring, "0123456789") == strlen(id->valuestring)) {
            snprintf(key, sizeof(key), "%s", id->valuestring);
        } else {
            continue;
        }
        museum_page_add(page, MUSEUM_MIA, key, number_of(cJSON_GetObjectItemCaseSensitive(source, "image_width")),
                        number_of(cJSON_GetObjectItemCaseSensitive(source, "image_height")));
    }
    // A full page means there may be another; the window's end is an empty one.
    page->more = page->items >= PAGE_ITEMS;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    const char *key = record->identity.museum.iiif_key;
    if (len == 0 || !museum_key_ok(key) || strspn(key, "0123456789") != strlen(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://1.api.artsmia.org/800/") && museum_put(out, len, &at, key) &&
                   museum_put(out, len, &at, ".jpg")
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

const museum_t museum_mia = {
    .id = "mia",
    .number = MUSEUM_MIA,
    .name = "Minneapolis Institute of Art",
    .page_items = PAGE_ITEMS,
    .offset_cap = 9950,  // a whole page inside the index's 10,000
    .response_bytes = 512 * 1024,
    .axes = "classification department country style",
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
