// SPDX-License-Identifier: Apache-2.0
// Rijksmuseum, Amsterdam. Pure.
//
//   listing  GET https://data.rijksmuseum.nl/search/collection
//                ?memberOfSetId=https://id.rijksmuseum.nl/{term}&imageAvailable=true
//            and then the address each page gives for the next (a token)
//   axes     set (the term is the number of one of the museum's sets)
//   key      the artwork's number: what follows "https://id.rijksmuseum.nl/"
//   picture  found by asking, in three steps (below)
//
// No API key. The answers are Linked Art (JSON-LD). A listing says only
// which artworks a set holds. Where an artwork's picture is takes a walk of
// three documents, each naming the next:
//
//   the artwork       https://id.rijksmuseum.nl/{key}   "shows"
//   what it shows     a VisualItem                      "digitally_shown_by"
//   its digital form  a DigitalObject                   "access_point"
//
// and the access point is an address at iiif.micr.io, whose first part
// names the picture: https://iiif.micr.io/{id}/full/!720,720/0/default.jpg.
// An id.rijksmuseum.nl address answers with a redirect to the document.
//
// 1.x did the walk once and then wrote the picture's id over the artwork's
// number in its record, with markers for "not walked yet" and "walk
// failed". In 2.0 a record's identity never changes (a fresh listing finds
// what the device already has by it), so the key stays the artwork's
// number and the walk is done when the picture is fetched: once, since the
// picture is then in the blob store.
//
// The walk takes the first thing each document names. Because listings
// page by token, a channel's offset is not honoured: it always starts at
// the top of the set.

#include <string.h>

#include "cJSON.h"
#include "museum.h"
#include "p3a_err.h"

#define ID_PREFIX     "https://id.rijksmuseum.nl/"
#define SEARCH_PREFIX "https://data.rijksmuseum.nl/search/collection?"
#define MICRIO_PREFIX "https://iiif.micr.io/"

static bool axis_ok(const char *axis)
{
    return strcmp(axis, "set") == 0;
}

static bool digits_only(const char *text)
{
    return text[0] != 0 && strspn(text, "0123456789") == strlen(text);
}

static esp_err_t list_url(const museum_query_t *q, char *out, size_t len)
{
    if (!axis_ok(q->axis) || q->term == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = 0;
    if (q->next != NULL && q->next[0] != 0) {
        // The address the page before gave. It is asked only if it is the same search.
        if (strncmp(q->next, SEARCH_PREFIX, strlen(SEARCH_PREFIX)) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        return museum_put(out, len, &at, q->next) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (!digits_only(q->term)) {
        return ESP_ERR_INVALID_ARG;
    }
    char set[128];
    size_t set_at = 0;
    set[0] = 0;
    if (!museum_put(set, sizeof(set), &set_at, ID_PREFIX) || !museum_put(set, sizeof(set), &set_at, q->term)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return museum_put(out, len, &at, SEARCH_PREFIX "memberOfSetId=") && museum_put_encoded(out, len, &at, set) &&
                   museum_put(out, len, &at, "&imageAvailable=true")
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t parse_page(const char *json, museum_page_t *page)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "orderedItems");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        page->items++;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(id) && strncmp(id->valuestring, ID_PREFIX, strlen(ID_PREFIX)) == 0 &&
            digits_only(id->valuestring + strlen(ID_PREFIX))) {
            museum_page_add(page, MUSEUM_RIJKS, id->valuestring + strlen(ID_PREFIX), 0, 0);
        }
    }
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "next"), "id");
    if (page->items > 0 && cJSON_IsString(next) && strlen(next->valuestring) < sizeof(page->next) &&
        strncmp(next->valuestring, SEARCH_PREFIX, strlen(SEARCH_PREFIX)) == 0) {
        strcpy(page->next, next->valuestring);
        page->more = true;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// The first address in a document's list `name` that is one of the museum's own documents.
static esp_err_t named_by(const char *json, const char *name, char *out, size_t len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, name), 0);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
    esp_err_t err = P3A_ERR_CORRUPT;
    if (cJSON_IsString(id) && strncmp(id->valuestring, ID_PREFIX, strlen(ID_PREFIX)) == 0 &&
        digits_only(id->valuestring + strlen(ID_PREFIX))) {
        size_t at = 0;
        out[0] = 0;
        err = museum_put(out, len, &at, id->valuestring) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return err;
}

// The picture's address, from the DigitalObject's access points.
static esp_err_t picture_of(const char *json, char *out, size_t len)
{
    cJSON *root = cJSON_Parse(json);
    esp_err_t err = P3A_ERR_CORRUPT;
    const cJSON *point;
    cJSON_ArrayForEach(point, cJSON_GetObjectItemCaseSensitive(root, "access_point")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(point, "id");
        if (!cJSON_IsString(id) || strncmp(id->valuestring, MICRIO_PREFIX, strlen(MICRIO_PREFIX)) != 0) {
            continue;
        }
        char picture[P3A_RECORD_IIIF_KEY_LEN];
        const char *start = id->valuestring + strlen(MICRIO_PREFIX);
        const size_t n = strcspn(start, "/?#");
        if (n == 0 || n >= sizeof(picture)) {
            continue;
        }
        memcpy(picture, start, n);
        picture[n] = 0;
        if (!museum_key_ok(picture)) {
            continue;
        }
        size_t at = 0;
        out[0] = 0;
        err = museum_put(out, len, &at, MICRIO_PREFIX) && museum_put(out, len, &at, picture) &&
                      museum_put(out, len, &at, "/full/!720,720/0/default.jpg")
                  ? ESP_OK
                  : ESP_ERR_INVALID_SIZE;
        break;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t walk(const p3a_record_t *record, int step, const char *json, char *out, size_t len, bool *done)
{
    *done = false;
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    switch (step) {
    case 0: {
        const char *key = record->identity.museum.iiif_key;
        if (!digits_only(key)) {
            return ESP_ERR_INVALID_ARG;
        }
        size_t at = 0;
        out[0] = 0;
        return museum_put(out, len, &at, ID_PREFIX) && museum_put(out, len, &at, key) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    case 1:
        return named_by(json, "shows", out, len);
    case 2:
        return named_by(json, "digitally_shown_by", out, len);
    case 3:
        *done = true;
        return picture_of(json, out, len);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

const museum_t museum_rijks = {
    .id = "rijks",
    .number = MUSEUM_RIJKS,
    .name = "Rijksmuseum",
    .page_items = 100,  // what a page holds; it is not asked for
    .offset_cap = 0,    // pages are reached by token only: a channel starts at the top
    .response_bytes = 256 * 1024,  // an artwork's document is 50 KB and more
    .accept = "application/ld+json",
    .axes = "set",
    .axis_ok = axis_ok,
    .list_url = list_url,
    .parse_page = parse_page,
    .walk = walk,
};
