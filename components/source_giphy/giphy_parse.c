// SPDX-License-Identifier: Apache-2.0
// Giphy's listing, read into records. See giphy_parse.h. Pure.

#include "giphy_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "p3a_err.h"

static const struct {
    const char *rendition;
    const char *suffix;
} s_files[GIPHY_FILE_END_] = {
    [GIPHY_FILE_DEFAULT] = {"fixed_height", "200.gif"},
    [GIPHY_FILE_200_GIF] = {"fixed_height", "200.gif"},
    [GIPHY_FILE_200_WEBP] = {"fixed_height", "200.webp"},
    [GIPHY_FILE_200W_GIF] = {"fixed_width", "200w.gif"},
    [GIPHY_FILE_200W_WEBP] = {"fixed_width", "200w.webp"},
    [GIPHY_FILE_ORIGINAL_GIF] = {"original", "giphy.gif"},
    [GIPHY_FILE_ORIGINAL_WEBP] = {"original", "giphy.webp"},
    [GIPHY_FILE_DOWNSIZED_MEDIUM] = {"downsized_medium", "giphy-downsized-medium.gif"},
    [GIPHY_FILE_DOWNSIZED] = {"downsized", "giphy-downsized.gif"},
};

giphy_file_t giphy_file_for(const char *rendition, bool webp)
{
    if (rendition != NULL) {
        if (strcmp(rendition, "fixed_width") == 0) {
            return webp ? GIPHY_FILE_200W_WEBP : GIPHY_FILE_200W_GIF;
        }
        if (strcmp(rendition, "original") == 0) {
            return webp ? GIPHY_FILE_ORIGINAL_WEBP : GIPHY_FILE_ORIGINAL_GIF;
        }
        if (strcmp(rendition, "downsized_medium") == 0) {
            return GIPHY_FILE_DOWNSIZED_MEDIUM;
        }
        if (strcmp(rendition, "downsized") == 0) {
            return GIPHY_FILE_DOWNSIZED;
        }
    }
    return webp ? GIPHY_FILE_200_WEBP : GIPHY_FILE_200_GIF;
}

const char *giphy_file_rendition(giphy_file_t file)
{
    return s_files[file < GIPHY_FILE_END_ ? file : GIPHY_FILE_DEFAULT].rendition;
}

// --- dates ----------------------------------------------------------------------

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

uint32_t giphy_parse_datetime(const char *text)
{
    int y, mo, d, h, mi, s;
    if (text == NULL || strlen(text) != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
        text[13] != ':' || text[16] != ':' || !digits(text, 4, &y) || !digits(text + 5, 2, &mo) ||
        !digits(text + 8, 2, &d) || !digits(text + 11, 2, &h) || !digits(text + 14, 2, &mi) ||
        !digits(text + 17, 2, &s)) {
        return 0;
    }
    if (y < 1970 || y > 2105 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
        return 0;
    }
    // Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
    // Not mktime(): that reads the time zone, and these dates are UTC.
    const int yy = mo <= 2 ? y - 1 : y;
    const int era = yy / 400;
    const int yoe = yy - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    const int64_t at = days * 86400 + h * 3600 + mi * 60 + s;
    return at > 0 && at <= UINT32_MAX ? (uint32_t)at : 0;
}

// --- a page ---------------------------------------------------------------------

// Giphy sends a rendition's sizes as strings ("480"), sometimes as numbers.
static uint16_t dimension(const cJSON *v)
{
    long n = 0;
    if (cJSON_IsString(v)) {
        n = strtol(v->valuestring, NULL, 10);
    } else if (cJSON_IsNumber(v)) {
        n = (long)v->valuedouble;
    }
    return n > 0 && n <= UINT16_MAX ? (uint16_t)n : 0;
}

// Which file the listing's downsized_medium really is, by the last part of
// its address: the dedicated file, the original, or neither (0).
static giphy_file_t downsized_file(const cJSON *url)
{
    if (!cJSON_IsString(url)) {
        return GIPHY_FILE_DEFAULT;
    }
    const char *s = url->valuestring;
    const size_t path_len = strcspn(s, "?#");
    size_t start = path_len;
    while (start > 0 && s[start - 1] != '/') {
        start--;
    }
    const size_t len = path_len - start;
    if (len == strlen("giphy-downsized-medium.gif") && strncmp(s + start, "giphy-downsized-medium.gif", len) == 0) {
        return GIPHY_FILE_DOWNSIZED_MEDIUM;
    }
    if (len == strlen("giphy.gif") && strncmp(s + start, "giphy.gif", len) == 0) {
        return GIPHY_FILE_ORIGINAL_GIF;
    }
    return GIPHY_FILE_DEFAULT;
}

// An id goes into an address: only what Giphy's ids are made of.
static bool id_ok(const char *id)
{
    const size_t len = strlen(id);
    if (len == 0 || len >= P3A_RECORD_GIPHY_ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool parse_item(const cJSON *gif, const giphy_parse_opts_t *opts, p3a_record_t *out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(gif, "id");
    if (!cJSON_IsString(id) || !id_ok(id->valuestring) || p3a_record_init_giphy(out, id->valuestring) != ESP_OK) {
        return false;
    }
    giphy_file_t file = opts->file < GIPHY_FILE_END_ && opts->file != GIPHY_FILE_DEFAULT ? opts->file : GIPHY_FILE_200_GIF;
    const cJSON *images = cJSON_GetObjectItemCaseSensitive(gif, "images");
    const cJSON *rendition = cJSON_GetObjectItemCaseSensitive(images, giphy_file_rendition(file));
    uint16_t w = dimension(cJSON_GetObjectItemCaseSensitive(rendition, "width"));
    uint16_t h = dimension(cJSON_GetObjectItemCaseSensitive(rendition, "height"));
    if (opts->prefer_downsized) {
        const cJSON *dm = cJSON_GetObjectItemCaseSensitive(images, "downsized_medium");
        const uint16_t dw = dimension(cJSON_GetObjectItemCaseSensitive(dm, "width"));
        const uint16_t dh = dimension(cJSON_GetObjectItemCaseSensitive(dm, "height"));
        const giphy_file_t dfile = downsized_file(cJSON_GetObjectItemCaseSensitive(dm, "url"));
        // Up to one and a half screens each way: the player scales a modest
        // oversize down, and the result still beats 200 pixels scaled up.
        if (dfile != GIPHY_FILE_DEFAULT && dw > 0 && dh > 0 && (uint32_t)dw * 2 <= (uint32_t)opts->screen_w * 3 &&
            (uint32_t)dh * 2 <= (uint32_t)opts->screen_h * 3) {
            file = dfile;
            w = dw;
            h = dh;
        }
    }
    out->identity.giphy.file = (uint8_t)file;
    out->width = w;
    out->height = h;
    const cJSON *trending = cJSON_GetObjectItemCaseSensitive(gif, "trending_datetime");
    const cJSON *imported = cJSON_GetObjectItemCaseSensitive(gif, "import_datetime");
    uint32_t at = cJSON_IsString(trending) ? giphy_parse_datetime(trending->valuestring) : 0;
    if (at == 0 && cJSON_IsString(imported)) {
        at = giphy_parse_datetime(imported->valuestring);
    }
    out->sort_at = at != 0 ? at : opts->now;
    return true;
}

esp_err_t giphy_parse_page(const char *json, const giphy_parse_opts_t *opts, p3a_record_t *out, uint32_t room,
                           uint32_t *count, uint32_t *items)
{
    *count = 0;
    *items = 0;
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *gif;
    cJSON_ArrayForEach(gif, data) {
        (*items)++;
        if (*count < room && parse_item(gif, opts, &out[*count])) {
            (*count)++;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// --- addresses ------------------------------------------------------------------

esp_err_t giphy_download_url(const p3a_record_t *record, char *out, size_t len)
{
    if (record->source != P3A_SOURCE_GIPHY || !id_ok(record->identity.giphy.id)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t file = record->identity.giphy.file < GIPHY_FILE_END_ ? record->identity.giphy.file : GIPHY_FILE_DEFAULT;
    const int n = snprintf(out, len, "https://i.giphy.com/media/%s/%s", record->identity.giphy.id, s_files[file].suffix);
    return n > 0 && (size_t)n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

// Appends to `out` at `*at`; false when it does not fit.
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

// The same, percent-encoded (RFC 3986: all but the unreserved characters).
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

esp_err_t giphy_list_url(const giphy_list_request_t *r, char *out, size_t len)
{
    if (r == NULL || r->api_key == NULL || r->api_key[0] == '\0' || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool search = r->query != NULL && r->query[0] != '\0';
    const giphy_file_t file = r->file < GIPHY_FILE_END_ ? r->file : GIPHY_FILE_DEFAULT;
    char number[40];
    size_t at = 0;
    out[0] = '\0';
    bool ok = put(out, len, &at, search ? "https://api.giphy.com/v1/gifs/search?api_key=" : "https://api.giphy.com/v1/gifs/trending?api_key=") &&
              put_encoded(out, len, &at, r->api_key);
    if (search) {
        ok = ok && put(out, len, &at, "&q=") && put_encoded(out, len, &at, r->query);
    }
    snprintf(number, sizeof(number), "&limit=%d&offset=%u", GIPHY_PAGE_ITEMS, (unsigned)r->offset);
    ok = ok && put(out, len, &at, number) && put(out, len, &at, "&rating=") &&
         put_encoded(out, len, &at, r->rating != NULL && r->rating[0] != '\0' ? r->rating : "pg-13") &&
         put(out, len, &at, "&fields=id,trending_datetime,import_datetime,images.") &&
         put(out, len, &at, giphy_file_rendition(file));
    // Both renditions, so that the better one can be chosen per GIF.
    if (r->prefer_downsized && file != GIPHY_FILE_DOWNSIZED_MEDIUM) {
        ok = ok && put(out, len, &at, ",images.downsized_medium");
    }
    if (r->random_id != NULL && r->random_id[0] != '\0') {
        ok = ok && put(out, len, &at, "&random_id=") && put_encoded(out, len, &at, r->random_id);
    }
    if (r->country != NULL && r->country[0] != '\0') {
        ok = ok && put(out, len, &at, "&country_code=") && put_encoded(out, len, &at, r->country);
    }
    return ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t giphy_random_id_url(const char *api_key, char *out, size_t len)
{
    size_t at = 0;
    if (api_key == NULL || api_key[0] == '\0' || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return put(out, len, &at, "https://api.giphy.com/v1/randomid?api_key=") && put_encoded(out, len, &at, api_key)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

// --- the two small answers --------------------------------------------------------

static esp_err_t copy_string(cJSON *root, const cJSON *value, char *out, size_t len)
{
    esp_err_t err = P3A_ERR_CORRUPT;
    if (cJSON_IsString(value) && value->valuestring[0] != '\0') {
        const size_t n = strlen(value->valuestring);
        err = n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK) {
            memcpy(out, value->valuestring, n + 1);
        }
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t giphy_parse_random_id(const char *json, char *out, size_t len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return copy_string(root, cJSON_GetObjectItemCaseSensitive(data, "random_id"), out, len);
}

esp_err_t giphy_click_lookup_url(const char *api_key, const p3a_record_t *record, const char *random_id, char *out,
                                 size_t len)
{
    if (api_key == NULL || api_key[0] == '\0' || len == 0 || record->source != P3A_SOURCE_GIPHY ||
        !id_ok(record->identity.giphy.id) || random_id == NULL || random_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t at = 0;
    out[0] = '\0';
    return put(out, len, &at, "https://api.giphy.com/v1/gifs/") && put(out, len, &at, record->identity.giphy.id) &&
                   put(out, len, &at, "?api_key=") && put_encoded(out, len, &at, api_key) &&
                   put(out, len, &at, "&fields=id,analytics&random_id=") && put_encoded(out, len, &at, random_id)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

// The address comes from a response: only one at Giphy, over https, is asked.
static bool giphy_address(const char *url)
{
    if (strncmp(url, "https://", 8) != 0) {
        return false;
    }
    const char *host = url + 8;
    const size_t len = strcspn(host, "/?#:@");
    static const char suffix[] = ".giphy.com";
    return host[len] == '/' && len > sizeof(suffix) - 1 &&
           strncmp(host + len - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0;
}

esp_err_t giphy_parse_click_url(const char *json, const char *random_id, uint64_t now_ms, char *out, size_t len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *analytics = cJSON_GetObjectItemCaseSensitive(data, "analytics");
    const cJSON *onclick = cJSON_GetObjectItemCaseSensitive(analytics, "onclick");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(onclick, "url");
    esp_err_t err = P3A_ERR_CORRUPT;
    if (cJSON_IsString(url) && giphy_address(url->valuestring) && len > 0) {
        char number[32];
        size_t at = 0;
        out[0] = '\0';
        snprintf(number, sizeof(number), "&ts=%llu", (unsigned long long)now_ms);
        err = put(out, len, &at, url->valuestring) && put(out, len, &at, strchr(url->valuestring, '?') ? "&" : "?") &&
                      put(out, len, &at, "random_id=") && put_encoded(out, len, &at, random_id) &&
                      put(out, len, &at, number)
                  ? ESP_OK
                  : ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return err;
}
