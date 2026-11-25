// SPDX-License-Identifier: Apache-2.0
// Smithsonian Open Access. Pure.
//
//   listing  GET https://api.si.edu/openaccess/api/v1.0/search
//                ?api_key={key}&q={query}&start=N&rows=50
//            where {query} is unit_code:{term} AND online_visual_material:true
//   axes     unit (the term is a unit's code: SAAM for the American Art
//            Museum, CHNDM for Cooper Hewitt, NPG, NMAAHC, HMSG, NMAfA)
//   key      the idsId of the record's first picture
//            (content.descriptiveNonRepeating.online_media.media, which is
//            a list or, with one picture, a single object)
//   picture  https://ids.si.edu/ids/iiif/{key}/full/!720,720/0/default.jpg
//
// The user's own API key, from api.data.gov (1,000 requests an hour). It
// is needed for listing only. A key that is not known is a 403. The
// server's firewall answers a request that does not say what client it is
// with an HTML page and status 200, which the parser refuses as not a
// listing. A record is 5 KB and more, so a page is 50. The listing gives
// the original's size, as text.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define PAGE_ITEMS 50

static bool axis_ok(const char *axis)
{
    return strcmp(axis, "unit") == 0;
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    if (!axis_ok(q->axis) || q->term == NULL || q->term[0] == 0 || q->api_key == NULL || q->api_key[0] == 0 || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // A unit's code is letters and digits. Anything else would be more query.
    for (const char *p = q->term; *p != 0; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    char query[160];
    const int n = snprintf(query, sizeof(query), "unit_code:%s AND online_visual_material:true", q->term);
    if (n < 0 || (size_t)n >= sizeof(query)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t at = 0;
    out[0] = 0;
    return museum_put(out, len, &at, "https://api.si.edu/openaccess/api/v1.0/search?api_key=") &&
                   museum_put_encoded(out, len, &at, q->api_key) && museum_put(out, len, &at, "&q=") &&
                   museum_put_encoded(out, len, &at, query) && museum_put(out, len, &at, "&start=") &&
                   museum_put_number(out, len, &at, q->start) && museum_put(out, len, &at, "&rows=") &&
                   museum_put_number(out, len, &at, PAGE_ITEMS)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

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
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "response"), "rows");
    if (!cJSON_IsArray(rows)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        page->items++;
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(row, "content");
        const cJSON *fixed = cJSON_GetObjectItemCaseSensitive(content, "descriptiveNonRepeating");
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(fixed, "online_media"), "media");
        const cJSON *first = cJSON_IsArray(media) ? cJSON_GetArrayItem(media, 0) : media;
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(first, "idsId");
        if (cJSON_IsString(ids)) {
            museum_page_add(page, MUSEUM_SI, ids->valuestring, size_of(cJSON_GetObjectItemCaseSensitive(first, "width")),
                            size_of(cJSON_GetObjectItemCaseSensitive(first, "height")));
        }
    }
    page->more = page->items >= PAGE_ITEMS;  // the answer does not say where it is in the whole
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t image_url(const p3a_record_t *record, char *out, size_t len)
{
    return museum_iiif_url("https://ids.si.edu/ids/iiif", record, out, len);
}

const museum_t museum_si = {
    .id = "si",
    .number = MUSEUM_SI,
    .name = "Smithsonian",
    .page_items = PAGE_ITEMS,
    .offset_cap = 10000,
    .response_bytes = 1024 * 1024,
    .key = MUSEUM_KEY_SMITHSONIAN,
    .axes = "unit",
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .image_url = image_url,
};
