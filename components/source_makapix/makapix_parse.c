// SPDX-License-Identifier: Apache-2.0
// Makapix Club's answers and p3a's questions. See makapix_parse.h. Pure.

#include "makapix_parse.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "p3a_err.h"

// --- small things -------------------------------------------------------------------

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool makapix_uuid_parse(const char *text, uint8_t out[16])
{
    if (text == NULL || strlen(text) != MAKAPIX_UUID_TEXT - 1) {
        return false;
    }
    int n = 0;
    for (int i = 0; i < MAKAPIX_UUID_TEXT - 1; i++) {
        const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen) {
            if (text[i] != '-') {
                return false;
            }
            continue;
        }
        const int hi = hex_value(text[i]), lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[n++] = (uint8_t)(hi * 16 + lo);
        i++;
    }
    return n == 16;
}

void makapix_uuid_text(const uint8_t uuid[16], char out[MAKAPIX_UUID_TEXT])
{
    static const char digits[] = "0123456789abcdef";
    int at = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[at++] = '-';
        }
        out[at++] = digits[uuid[i] >> 4];
        out[at++] = digits[uuid[i] & 0x0F];
    }
    out[at] = '\0';
}

static bool digits_at(const char *s, int n, int *value)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    *value = v;
    return true;
}

uint32_t makapix_time(const char *text)
{
    // YYYY-MM-DDTHH:MM:SS, then an optional fraction, then Z or +00:00.
    if (text == NULL || strlen(text) < 20) {
        return 0;
    }
    int year, month, day, hour, minute, second;
    if (!digits_at(text, 4, &year) || text[4] != '-' || !digits_at(text + 5, 2, &month) || text[7] != '-' ||
        !digits_at(text + 8, 2, &day) || (text[10] != 'T' && text[10] != ' ') || !digits_at(text + 11, 2, &hour) ||
        text[13] != ':' || !digits_at(text + 14, 2, &minute) || text[16] != ':' || !digits_at(text + 17, 2, &second)) {
        return 0;
    }
    const char *rest = text + 19;
    if (*rest == '.') {
        rest++;
        if (*rest < '0' || *rest > '9') {
            return 0;
        }
        while (*rest >= '0' && *rest <= '9') {
            rest++;
        }
    }
    if (strcmp(rest, "Z") != 0 && strcmp(rest, "+00:00") != 0) {
        return 0;
    }
    if (year < 2000 || year > 2105 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60) {
        return 0;
    }
    // Days from 1970-01-01 (the civil-from-days arithmetic, for the proleptic Gregorian calendar).
    const int y = month <= 2 ? year - 1 : year;
    const int era = y / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    return seconds > 0 && seconds <= UINT32_MAX ? (uint32_t)seconds : 0;
}

// --- channels -----------------------------------------------------------------------

static const char *const s_channels[MAKAPIX_CHANNEL_COUNT] = {"all", "promoted", "user", "by_user", "hashtag", "reactions"};

static bool takes_identifier(makapix_channel_t channel)
{
    return channel == MAKAPIX_CHANNEL_BY_USER || channel == MAKAPIX_CHANNEL_HASHTAG || channel == MAKAPIX_CHANNEL_REACTIONS;
}

bool makapix_channel_parse(const char *kind, const char *identifier, makapix_channel_t *out)
{
    if (kind == NULL) {
        return false;
    }
    for (int i = 0; i < MAKAPIX_CHANNEL_COUNT; i++) {
        if (strcmp(kind, s_channels[i]) != 0) {
            continue;
        }
        if (takes_identifier((makapix_channel_t)i)) {
            // A sqid or a tag: it goes into a JSON string as it is.
            if (identifier == NULL || identifier[0] == '\0' || strlen(identifier) > 64) {
                return false;
            }
            for (const char *p = identifier; *p != '\0'; p++) {
                const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                                *p == '_' || *p == '-';
                if (!ok) {
                    return false;
                }
            }
        }
        *out = (makapix_channel_t)i;
        return true;
    }
    return false;
}

const char *makapix_channel_name(makapix_channel_t channel)
{
    return channel < MAKAPIX_CHANNEL_COUNT ? s_channels[channel] : NULL;
}

// --- asking -------------------------------------------------------------------------

// A cursor is the server's own text handed back: base64 or digits. Anything
// else is not sent.
static bool cursor_ok(const char *cursor)
{
    if (strlen(cursor) >= MAKAPIX_CURSOR_LEN) {
        return false;
    }
    for (const char *p = cursor; *p != '\0'; p++) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' ||
                        *p == '/' || *p == '=' || *p == '-' || *p == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

esp_err_t makapix_query_body(makapix_channel_t channel, const char *identifier, const char *cursor, char *out,
                             size_t len)
{
    const char *name = makapix_channel_name(channel);
    if (name == NULL || (cursor != NULL && !cursor_ok(cursor))) {
        return ESP_ERR_INVALID_ARG;
    }
    char who[96] = "";
    if (takes_identifier(channel)) {
        // Checked by makapix_channel_parse(): nothing in it needs escaping.
        snprintf(who, sizeof(who), ",\"%s\":\"%s\"", channel == MAKAPIX_CHANNEL_HASHTAG ? "hashtag" : "user_sqid",
                 identifier != NULL ? identifier : "");
    }
    char from[MAKAPIX_CURSOR_LEN + 16] = "";
    if (cursor != NULL && cursor[0] != '\0') {
        snprintf(from, sizeof(from), ",\"cursor\":\"%s\"", cursor);
    }
    const int n = snprintf(out, len,
                           "{\"request_type\":\"query_posts\",\"channel\":\"%s\"%s,\"sort\":\"server_order\","
                           "\"limit\":%d%s,\"include_fields\":[\"artwork_modified_at\",\"width\",\"height\"]}",
                           name, who, MAKAPIX_PAGE_POSTS, from);
    return n > 0 && (size_t)n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t makapix_feed_url(const char *cursor, char *out, size_t len)
{
    if (cursor != NULL && !cursor_ok(cursor)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, len,
                     "https://" MAKAPIX_HOST "/api/v1/feed/promoted?limit=%d&fields=id,kind,storage_key,art_url,"
                     "width,height,artwork_modified_at,promoted_at,listed_at,created_at",
                     MAKAPIX_PAGE_POSTS);
    if (n <= 0 || (size_t)n >= len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (cursor == NULL || cursor[0] == '\0') {
        return ESP_OK;
    }
    // Base64 holds '+', '/' and '=', which mean something else in a query.
    size_t at = (size_t)n;
    static const char prefix[] = "&cursor=";
    if (at + sizeof(prefix) > len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out + at, prefix, sizeof(prefix));
    at += sizeof(prefix) - 1;
    for (const char *p = cursor; *p != '\0'; p++) {
        const bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                           *p == '-' || *p == '_';
        if (at + 4 > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (plain) {
            out[at++] = *p;
        } else {
            at += (size_t)snprintf(out + at, 4, "%%%02X", (unsigned char)*p);
        }
    }
    out[at] = '\0';
    return ESP_OK;
}

esp_err_t makapix_reaction_body(uint32_t post_id, bool revoke, char *out, size_t len)
{
    if (post_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // The thumbs-up, U+1F44D, as UTF-8.
    const int n = snprintf(out, len, "{\"request_type\":\"%s\",\"post_id\":%u,\"emoji\":\"\xF0\x9F\x91\x8D\"}",
                           revoke ? "revoke_reaction" : "submit_reaction", (unsigned)post_id);
    return n > 0 && (size_t)n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

// --- reading ------------------------------------------------------------------------

static const char *text_of(const cJSON *object, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static uint32_t number_of(const cJSON *object, const char *name, uint32_t max)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > (double)max ||
        v->valuedouble != (double)(uint32_t)v->valuedouble) {
        return 0;
    }
    return (uint32_t)v->valuedouble;
}

static p3a_format_t format_named(const char *name, size_t n)
{
    static const struct {
        const char *name;
        p3a_format_t format;
    } known[] = {{"webp", P3A_FORMAT_WEBP}, {"gif", P3A_FORMAT_GIF}, {"png", P3A_FORMAT_PNG}, {"bmp", P3A_FORMAT_BMP}};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strlen(known[i].name) == n && memcmp(known[i].name, name, n) == 0) {
            return known[i].format;
        }
    }
    return P3A_FORMAT_UNKNOWN;
}

// The post's native format: what its address ends in, else what it says.
static p3a_format_t format_of(const cJSON *post)
{
    const char *url = text_of(post, "art_url");
    if (url != NULL) {
        const size_t end = strcspn(url, "?#");
        size_t dot = end;
        while (dot > 0 && url[dot - 1] != '.' && url[dot - 1] != '/') {
            dot--;
        }
        if (dot > 0 && url[dot - 1] == '.') {
            const p3a_format_t format = format_named(url + dot, end - dot);
            if (format != P3A_FORMAT_UNKNOWN) {
                return format;
            }
        }
    }
    const char *native = text_of(post, "native_format");
    return native != NULL ? format_named(native, strlen(native)) : P3A_FORMAT_UNKNOWN;
}

static bool parse_post(const cJSON *post, bool promoted, p3a_record_t *out)
{
    if (!cJSON_IsObject(post)) {
        return false;
    }
    const char *kind = text_of(post, "kind");
    if (kind != NULL && strcmp(kind, "artwork") != 0) {
        return false;  // a playlist: nothing p3a can play by itself
    }
    uint32_t id = number_of(post, "post_id", UINT32_MAX);
    if (id == 0) {
        id = number_of(post, "id", UINT32_MAX);
    }
    uint8_t uuid[16];
    const p3a_format_t format = format_of(post);
    if (id == 0 || !makapix_uuid_parse(text_of(post, "storage_key"), uuid) || format == P3A_FORMAT_UNKNOWN ||
        p3a_record_init_makapix(out, id, uuid) != ESP_OK) {
        return false;
    }
    out->format = (uint8_t)format;
    // What the channel is ordered by: when it was promoted, on that
    // channel; when it was (last) listed, on the others. Older posts carry
    // only when they were made.
    uint32_t at = promoted ? makapix_time(text_of(post, "promoted_at")) : 0;
    if (at == 0) {
        at = makapix_time(text_of(post, "listed_at"));
    }
    if (at == 0) {
        at = makapix_time(text_of(post, "created_at"));
    }
    out->sort_at = at;
    out->modified_at = makapix_time(text_of(post, "artwork_modified_at"));
    // Makapix knows the canvas: at most 256 each way.
    const uint32_t w = number_of(post, "width", 4096), h = number_of(post, "height", 4096);
    if (w != 0 && h != 0) {
        out->width = (uint16_t)w;
        out->height = (uint16_t)h;
    }
    return true;
}

static esp_err_t refusal(const cJSON *root)
{
    const char *code = text_of(root, "error_code");
    if (code == NULL) {
        return ESP_FAIL;
    }
    if (strcmp(code, "user_not_found") == 0 || strcmp(code, "not_found") == 0 || strcmp(code, "deleted") == 0 ||
        strcmp(code, "invalid_hashtag") == 0 || strcmp(code, "not_visible") == 0 || strcmp(code, "not_available") == 0) {
        return P3A_ERR_MISSING;
    }
    if (strcmp(code, "authentication_failed") == 0) {
        return P3A_ERR_AUTH;
    }
    if (strcmp(code, "rate_limited") == 0 || strcmp(code, "rate_limit_exceeded") == 0) {
        return P3A_ERR_RATE_LIMITED;
    }
    return ESP_FAIL;
}

esp_err_t makapix_parse_page(const char *json, bool promoted, p3a_record_t *out, uint32_t room, makapix_page_t *page)
{
    memset(page, 0, sizeof(*page));
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    esp_err_t err = ESP_OK;
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    const cJSON *posts = cJSON_GetObjectItemCaseSensitive(root, "posts");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *list = NULL;
    if (cJSON_IsFalse(success)) {
        err = refusal(root);
    } else if (cJSON_IsTrue(success) && cJSON_IsArray(posts)) {
        list = posts;  // the player's own question
    } else if (success == NULL && cJSON_IsArray(items)) {
        list = items;  // the public feed
    } else {
        err = P3A_ERR_CORRUPT;
    }
    if (list != NULL) {
        const cJSON *post = NULL;
        cJSON_ArrayForEach(post, list)
        {
            page->items++;
            if (page->count < room && parse_post(post, promoted, &out[page->count])) {
                page->count++;
            }
        }
        const char *cursor = text_of(root, "next_cursor");
        if (cursor != NULL && cursor[0] != '\0' && cursor_ok(cursor)) {
            strcpy(page->cursor, cursor);
        }
        // The feed has no "has_more": a cursor is its word that there is more.
        const cJSON *more = cJSON_GetObjectItemCaseSensitive(root, "has_more");
        page->has_more = page->cursor[0] != '\0' && (list == items || cJSON_IsTrue(more));
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t makapix_parse_result(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return P3A_ERR_CORRUPT;
    }
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    const esp_err_t err = cJSON_IsTrue(success) ? ESP_OK : cJSON_IsFalse(success) ? refusal(root) : (esp_err_t)P3A_ERR_CORRUPT;
    cJSON_Delete(root);
    return err;
}

esp_err_t makapix_parse_token(const char *json, char *out, size_t len)
{
    out[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    const char *token = cJSON_IsObject(root) ? text_of(root, "api_token") : NULL;
    esp_err_t err = P3A_ERR_CORRUPT;
    if (token != NULL && token[0] != '\0' && strlen(token) < len) {
        // It goes into a header: nothing in it may end one.
        bool clean = true;
        for (const char *p = token; *p != '\0'; p++) {
            clean = clean && (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7F;
        }
        if (clean) {
            strcpy(out, token);
            err = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return err;
}

// --- registration -------------------------------------------------------------------

// A host name as it may go into an address: letters, digits, '.', '-'.
static bool host_ok(const char *host, size_t max)
{
    if (host == NULL || host[0] == '\0' || strlen(host) >= max) {
        return false;
    }
    for (const char *p = host; *p != '\0'; p++) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' ||
                        *p == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool broker_of(const cJSON *broker, char host[64], uint16_t *port)
{
    const char *name = cJSON_IsObject(broker) ? text_of(broker, "host") : NULL;
    const uint32_t number = cJSON_IsObject(broker) ? number_of(broker, "port", 65535) : 0;
    if (!host_ok(name, 64) || number == 0) {
        return false;
    }
    strcpy(host, name);
    *port = (uint16_t)number;
    return true;
}

esp_err_t makapix_parse_provision(const char *json, makapix_provision_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    esp_err_t err = P3A_ERR_CORRUPT;
    if (cJSON_IsObject(root)) {
        const char *key = text_of(root, "player_key");
        const char *code = text_of(root, "registration_code");
        uint8_t uuid[16];
        bool code_ok = code != NULL && strlen(code) >= 4 && strlen(code) < MAKAPIX_CODE_LEN;
        for (const char *p = code; code_ok && *p != '\0'; p++) {
            code_ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        }
        if (makapix_uuid_parse(key, uuid) && code_ok &&
            broker_of(cJSON_GetObjectItemCaseSensitive(root, "mqtt_broker"), out->host, &out->port)) {
            makapix_uuid_text(uuid, out->player_key);  // in the one spelling the server uses
            strcpy(out->code, code);
            out->expires_at = makapix_time(text_of(root, "registration_code_expires_at"));
            err = ESP_OK;
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
    }
    return err;
}

esp_err_t makapix_parse_credentials(const char *json, char *pems, size_t pems_len, makapix_credentials_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    esp_err_t err = P3A_ERR_CORRUPT;
    if (cJSON_IsObject(root)) {
        static const char *const names[3] = {"ca_pem", "cert_pem", "key_pem"};
        const char *got[3] = {NULL, NULL, NULL};
        size_t at = 0;
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            const char *pem = text_of(root, names[i]);
            const size_t len = pem != NULL ? strlen(pem) : 0;
            ok = len > 0 && len < 4096 && strncmp(pem, "-----BEGIN ", 11) == 0 && at + len + 1 <= pems_len;
            if (ok) {
                memcpy(pems + at, pem, len + 1);
                got[i] = pems + at;
                at += len + 1;
            }
        }
        if (ok) {
            out->ca = got[0];
            out->cert = got[1];
            out->key = got[2];
            (void)broker_of(cJSON_GetObjectItemCaseSensitive(root, "broker"), out->host, &out->port);
            const char *token = text_of(root, "api_token");
            if (token != NULL && token[0] != '\0' && strlen(token) < sizeof(out->token)) {
                bool clean = true;
                for (const char *p = token; *p != '\0'; p++) {
                    clean = clean && (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7F;
                }
                if (clean) {
                    strcpy(out->token, token);
                }
            }
            err = ESP_OK;
        }
    }
    // The parse tree held the private key.
    if (root != NULL) {
        cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key_pem");
        if (cJSON_IsString(key) && key->valuestring != NULL) {
            memset(key->valuestring, 0, strlen(key->valuestring));
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        memset(pems, 0, pems_len);
    }
    return err;
}

// --- the file -----------------------------------------------------------------------

esp_err_t makapix_vault_url(const p3a_record_t *record, const uint8_t digest[32], char *out, size_t len)
{
    static const char *const extensions[P3A_FORMAT_END_] = {
        [P3A_FORMAT_WEBP] = "webp", [P3A_FORMAT_GIF] = "gif", [P3A_FORMAT_PNG] = "png", [P3A_FORMAT_BMP] = "bmp"};
    if (record == NULL || record->source != P3A_SOURCE_MAKAPIX || record->format >= P3A_FORMAT_END_ ||
        extensions[record->format] == NULL) {
        return P3A_ERR_MISSING;
    }
    char key[MAKAPIX_UUID_TEXT];
    makapix_uuid_text(record->identity.makapix.uuid, key);
    const int n = snprintf(out, len, "https://" MAKAPIX_VAULT_HOST "/%02x/%02x/%s.%s", digest[0] & 0x3F, digest[1] & 0x3F,
                           key, extensions[record->format]);
    return n > 0 && (size_t)n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
