// SPDX-License-Identifier: Apache-2.0
// Klipy's listings, read into records. See klipy_parse.h. Pure.

#include "klipy_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "p3a_err.h"

bool klipy_kind_parse(const char *kind, uint8_t *product, klipy_mode_t *mode)
{
    if (kind == NULL) {
        return false;
    }
    const char *colon = strchr(kind, ':');
    const char *what = kind;
    *product = KLIPY_PRODUCT_GIF;
    if (colon != NULL) {
        const size_t len = (size_t)(colon - kind);
        if (len == 7 && strncmp(kind, "sticker", 7) == 0) {
            *product = KLIPY_PRODUCT_STICKER;
        } else if (!(len == 3 && strncmp(kind, "gif", 3) == 0)) {
            return false;
        }
        what = colon + 1;
    }
    if (what[0] == '\0' || strcmp(what, "trending") == 0) {
        *mode = KLIPY_MODE_TRENDING;
    } else if (strcmp(what, "search") == 0 || strcmp(what, "category") == 0) {
        *mode = KLIPY_MODE_SEARCH;
    } else {
        return false;
    }
    return true;
}

static uint16_t dimension(const cJSON *v)
{
    double n = 0;
    if (cJSON_IsNumber(v)) {
        n = v->valuedouble;
    } else if (cJSON_IsString(v)) {
        n = (double)strtol(v->valuestring, NULL, 10);
    }
    return n >= 1 && n <= UINT16_MAX ? (uint16_t)n : 0;
}

typedef struct {
    const char *url;  // into the JSON tree
    uint16_t w, h;
} choice_t;

// Only an https address at Klipy's own hosts is fetched: the address comes
// from a response, and a response is not trusted to send p3a anywhere.
static bool address_ok(const char *url)
{
    if (strncmp(url, "https://", 8) != 0) {
        return false;
    }
    const char *host = url + 8;
    const size_t len = strcspn(host, "/?#:@");
    static const char suffix[] = ".klipy.com";
    return host[len] == '/' && len > sizeof(suffix) - 1 &&
           strncmp(host + len - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0;
}

// The file to take of an item's "file" object.
static bool pick(const cJSON *file, const klipy_parse_opts_t *opts, choice_t *out)
{
    static const char *const tiers[] = {"hd", "md", "sm", "xs"};
    const char *const formats[2] = {opts->webp ? "webp" : "gif", opts->webp ? "gif" : "webp"};
    if (!cJSON_IsObject(file)) {
        return false;
    }
    for (int f = 0; f < 2; f++) {
        choice_t fit = {0}, any = {0};
        uint32_t fit_area = 0, any_area = 0;
        for (int t = 0; t < 4; t++) {
            const cJSON *one = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(file, tiers[t]), formats[f]);
            const cJSON *url = cJSON_GetObjectItemCaseSensitive(one, "url");
            if (!cJSON_IsString(url) || !address_ok(url->valuestring)) {
                continue;
            }
            const choice_t c = {.url = url->valuestring,
                                .w = dimension(cJSON_GetObjectItemCaseSensitive(one, "width")),
                                .h = dimension(cJSON_GetObjectItemCaseSensitive(one, "height"))};
            const uint32_t area = (uint32_t)c.w * c.h;
            if (any.url == NULL || area > any_area) {
                any = c;
                any_area = area;
            }
            if ((uint32_t)c.w * 2 <= (uint32_t)opts->screen_w * 3 && (uint32_t)c.h * 2 <= (uint32_t)opts->screen_h * 3 &&
                (fit.url == NULL || area > fit_area)) {
                fit = c;
                fit_area = area;
            }
        }
        if (fit.url != NULL || any.url != NULL) {
            *out = fit.url != NULL ? fit : any;
            return true;
        }
    }
    return false;
}

static uint64_t item_id(const cJSON *item)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
    if (cJSON_IsNumber(id)) {
        // Klipy's ids have 16 digits: exact in a double, which holds 2^53.
        return id->valuedouble >= 1 && id->valuedouble < 9007199254740992.0 ? (uint64_t)id->valuedouble : 0;
    }
    if (cJSON_IsString(id)) {
        char *end = NULL;
        const unsigned long long n = strtoull(id->valuestring, &end, 10);
        return end != id->valuestring && *end == '\0' ? (uint64_t)n : 0;
    }
    return 0;
}

static bool parse_item(const cJSON *item, const klipy_parse_opts_t *opts, p3a_record_t *out)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    if (!cJSON_IsObject(item) || (cJSON_IsString(type) && strcmp(type->valuestring, "ad") == 0)) {
        return false;
    }
    choice_t choice;
    const uint64_t id = item_id(item);
    if (id == 0 || !pick(cJSON_GetObjectItemCaseSensitive(item, "file"), opts, &choice) ||
        p3a_record_init_klipy(out, id, opts->product) != ESP_OK) {
        return false;
    }
    out->width = choice.w;
    out->height = choice.h;
    out->sort_at = opts->now;
    return true;
}

// The array of items in an answer: data.data.
static const cJSON *items_of(const cJSON *root, const cJSON **data_out)
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(data, "data");
    if (data_out != NULL) {
        *data_out = data;
    }
    return cJSON_IsArray(array) ? array : NULL;
}

esp_err_t klipy_parse_page(const char *json, const klipy_parse_opts_t *opts, p3a_record_t *out, uint32_t room,
                           uint32_t *count, uint32_t *items, bool *has_next)
{
    *count = 0;
    *items = 0;
    *has_next = false;
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = NULL;
    const cJSON *array = items_of(root, &data);
    if (array == NULL) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        (*items)++;
        if (*count < room && parse_item(item, opts, &out[*count])) {
            (*count)++;
        }
    }
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(data, "has_next");
    *has_next = cJSON_IsBool(next) ? cJSON_IsTrue(next) : *items >= KLIPY_PAGE_ITEMS;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t klipy_parse_item_url(const char *json, const klipy_parse_opts_t *opts, char *out, size_t len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *array = items_of(root, NULL);
    esp_err_t err = P3A_ERR_CORRUPT;
    if (array != NULL) {
        choice_t choice;
        if (cJSON_GetArraySize(array) == 0) {
            err = P3A_ERR_MISSING;
        } else if (pick(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(array, 0), "file"), opts, &choice)) {
            const size_t n = strlen(choice.url);
            err = n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
            if (err == ESP_OK) {
                memcpy(out, choice.url, n + 1);
            }
        }
    }
    cJSON_Delete(root);
    return err;
}

// --- addresses ------------------------------------------------------------------

static bool put(char *out, size_t len, size_t *at, const char *text)
{
    const size_t n = strlen(text);
    if (*at + n >= len) {
        return false;
    }
    memcpy(out + *at, text, n + 1);
    *at += n;
    return true;
}

static bool put_encoded(char *out, size_t len, size_t *at, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        const unsigned char c = *p;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
                           c == '_' || c == '.' || c == '~';
        const char piece[4] = {plain ? (char)c : '%', plain ? '\0' : hex[c >> 4], plain ? '\0' : hex[c & 15], '\0'};
        if (!put(out, len, at, piece)) {
            return false;
        }
    }
    return true;
}

static bool put_base(char *out, size_t len, size_t *at, const char *api_key, uint8_t product)
{
    out[0] = '\0';
    // The key is a part of the path: encoded, it cannot add a part of its own.
    return put(out, len, at, "https://api.klipy.com/api/v1/") && put_encoded(out, len, at, api_key) &&
           put(out, len, at, product == KLIPY_PRODUCT_STICKER ? "/stickers/" : "/gifs/");
}

esp_err_t klipy_list_url(const char *api_key, uint8_t product, const char *query, uint32_t page, const char *rating,
                         char *out, size_t len)
{
    if (api_key == NULL || api_key[0] == '\0' || len == 0 || page == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool search = query != NULL && query[0] != '\0';
    char number[48];
    size_t at = 0;
    bool ok = put_base(out, len, &at, api_key, product);
    if (search) {
        ok = ok && put(out, len, &at, "search?q=") && put_encoded(out, len, &at, query) && put(out, len, &at, "&");
    } else {
        ok = ok && put(out, len, &at, "trending?");
    }
    snprintf(number, sizeof(number), "page=%u&per_page=%d&rating=", (unsigned)page, KLIPY_PAGE_ITEMS);
    ok = ok && put(out, len, &at, number) &&
         put_encoded(out, len, &at, rating != NULL && rating[0] != '\0' ? rating : "pg-13");
    return ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t klipy_item_url(const char *api_key, const p3a_record_t *record, char *out, size_t len)
{
    if (api_key == NULL || api_key[0] == '\0' || len == 0 || record->source != P3A_SOURCE_KLIPY ||
        record->identity.klipy.id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char number[40];
    size_t at = 0;
    snprintf(number, sizeof(number), "items?ids=%llu", (unsigned long long)record->identity.klipy.id);
    return put_base(out, len, &at, api_key, record->identity.klipy.product) && put(out, len, &at, number)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}
