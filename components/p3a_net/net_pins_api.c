// SPDX-License-Identifier: Apache-2.0
// The pin lists over HTTP (p3a_pins.h).
//
//   GET    /api/v2/pins                  the lists: slug, name, active, count
//   POST   /api/v2/pins                  {"name"}: a new list; answers its slug
//   GET    /api/v2/pins/{slug}           a list's pins, ?offset= and ?limit= (at most 64 a page)
//   PUT    /api/v2/pins/{slug}           {"name"} renames it, {"active": true} makes it the active one
//   DELETE /api/v2/pins/{slug}           the list and its pins; not the active list
//   POST   /api/v2/pins/{slug}/pin       pins the artwork on the screen
//   POST   /api/v2/pins/{slug}/unpin     {} unpins the artwork on the screen;
//                                        {"position", "key"} unpins that entry of the list
//
// Where a slug is asked for, "active" names the active list.
//
// An entry is named by its position and its key together: a position alone
// would name another pin once the list has changed, and a key alone is a
// 32-bit number that two artworks may share. An unpin whose two do not
// match the list is refused, and the page reads the list again.

#include <stdlib.h>
#include <string.h>

#include "net_internal.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_pins.h"
#include "play_scheduler.h"
#include "psram_alloc.h"

#define PAGE_MAX 64

// The slug in the address, or NULL for "active". False (answered) for anything else.
static bool slug_param(p3a_http_request_t *req, char *buf, size_t len, const char **slug)
{
    if (p3a_http_param(req, 0, buf, len)) {
        if (strcmp(buf, "active") == 0) {
            *slug = NULL;
            return true;
        }
        if (p3a_pin_slug_ok(buf)) {
            *slug = buf;
            return true;
        }
    }
    p3a_http_fail(req, 404, "NO_SUCH_LIST", "there is no such pin list");
    return false;
}

static void fail_pins(p3a_http_request_t *req, esp_err_t err, const char *what)
{
    if (err == (esp_err_t)P3A_ERR_MISSING) {
        p3a_http_fail(req, 404, "NO_SUCH_LIST", "there is no such pin list");
    } else {
        p3a_http_fail_err(req, err, what);
    }
}

static void get_lists(p3a_http_request_t *req)
{
    p3a_pin_list_info_t *lists = psram_calloc(P3A_PINS_MAX_LISTS, sizeof(*lists));
    size_t count = 0;
    const esp_err_t err = lists != NULL ? p3a_pins_lists(lists, P3A_PINS_MAX_LISTS, &count) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        free(lists);
        p3a_http_fail_err(req, err, "reading the pin lists");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(data, "lists");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "slug", lists[i].slug);
        cJSON_AddStringToObject(item, "name", lists[i].name);
        cJSON_AddBoolToObject(item, "active", lists[i].active);
        cJSON_AddNumberToObject(item, "count", lists[i].count);
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddNumberToObject(data, "max_lists", P3A_PINS_MAX_LISTS);
    cJSON_AddNumberToObject(data, "max_pins", P3A_PIN_LIST_MAX);
    free(lists);
    p3a_http_ok(req, data);
}

static void post_list(p3a_http_request_t *req)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "name");
    if (!cJSON_IsString(name) || !p3a_pin_name_ok(name->valuestring)) {
        p3a_http_fail(req, 400, "INVALID_NAME", "a list's name is 1 to %d bytes", P3A_PIN_NAME_LEN - 1);
        return;
    }
    char slug[P3A_PIN_SLUG_LEN];
    const esp_err_t err = p3a_pins_create(name->valuestring, slug);
    if (err == ESP_ERR_NO_MEM) {
        p3a_http_fail(req, 409, "TOO_MANY_LISTS", "there are %d pin lists already; delete one first", P3A_PINS_MAX_LISTS);
    } else if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "making the list");
    } else {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "slug", slug);
        p3a_http_ok(req, data);
    }
}

static uint32_t query_number(p3a_http_request_t *req, const char *key, uint32_t otherwise)
{
    char text[16];
    if (!p3a_http_query(req, key, text, sizeof(text)) || text[0] == '\0') {
        return otherwise;
    }
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    return (*end == '\0') ? (uint32_t)value : otherwise;
}

static void get_list(p3a_http_request_t *req)
{
    char buf[P3A_PIN_SLUG_LEN + 8];
    const char *slug = NULL;
    if (!slug_param(req, buf, sizeof(buf), &slug)) {
        return;
    }
    p3a_index_t index = {0};
    esp_err_t err = p3a_index_create(&index, P3A_PIN_LIST_MAX);
    if (err == ESP_OK) {
        err = p3a_pins_load(slug, &index);
    }
    if (err != ESP_OK) {
        p3a_index_destroy(&index);
        fail_pins(req, err, "reading the list");
        return;
    }
    const uint32_t offset = query_number(req, "offset", 0);
    uint32_t limit = query_number(req, "limit", PAGE_MAX);
    limit = limit > PAGE_MAX ? PAGE_MAX : limit;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "count", index.count);
    cJSON_AddNumberToObject(data, "offset", offset);
    cJSON *array = cJSON_AddArrayToObject(data, "pins");
    for (uint32_t i = offset; i < index.count && i - offset < limit; i++) {
        cJSON *item = net_record_json(&index.records[i]);
        cJSON_AddNumberToObject(item, "position", i);
        cJSON_AddNumberToObject(item, "key", index.records[i].id32);
        cJSON_AddItemToArray(array, item);
    }
    p3a_index_destroy(&index);
    p3a_http_ok(req, data);
}

static void put_list(p3a_http_request_t *req)
{
    char buf[P3A_PIN_SLUG_LEN + 8];
    const char *slug = NULL;
    if (!slug_param(req, buf, sizeof(buf), &slug)) {
        return;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "name");
    const cJSON *active = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "active");
    if (slug == NULL || (name == NULL && !cJSON_IsTrue(active))) {
        p3a_http_fail(req, 400, "INVALID_REQUEST", "name a list by its slug, and give \"name\" or \"active\": true");
        return;
    }
    if (name != NULL && (!cJSON_IsString(name) || !p3a_pin_name_ok(name->valuestring))) {
        p3a_http_fail(req, 400, "INVALID_NAME", "a list's name is 1 to %d bytes", P3A_PIN_NAME_LEN - 1);
        return;
    }
    esp_err_t err = name != NULL ? p3a_pins_rename(slug, name->valuestring) : ESP_OK;
    if (err == ESP_OK && cJSON_IsTrue(active)) {
        err = p3a_pins_set_active(slug);
    }
    err == ESP_OK ? p3a_http_ok(req, NULL) : fail_pins(req, err, "changing the list");
}

static void delete_list(p3a_http_request_t *req)
{
    char buf[P3A_PIN_SLUG_LEN + 8];
    const char *slug = NULL;
    if (!slug_param(req, buf, sizeof(buf), &slug)) {
        return;
    }
    const esp_err_t err = slug != NULL ? p3a_pins_delete(slug) : ESP_ERR_INVALID_STATE;
    if (err == ESP_ERR_INVALID_STATE) {
        p3a_http_fail(req, 409, "LIST_IS_ACTIVE", "the active list cannot be deleted; make another one active first");
    } else {
        err == ESP_OK ? p3a_http_ok(req, NULL) : fail_pins(req, err, "deleting the list");
    }
}

static bool on_screen(p3a_http_request_t *req, p3a_record_t *record)
{
    if (play_scheduler_current(record)) {
        return true;
    }
    p3a_http_fail(req, 409, "NOTHING_SHOWN", "no artwork is on the screen");
    return false;
}

static void post_pin(p3a_http_request_t *req)
{
    char buf[P3A_PIN_SLUG_LEN + 8];
    const char *slug = NULL;
    p3a_record_t record;
    if (!slug_param(req, buf, sizeof(buf), &slug) || !on_screen(req, &record)) {
        return;
    }
    const esp_err_t err = p3a_pins_pin(slug, &record);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        p3a_http_fail(req, 409, "CANNOT_BE_PINNED", "a file of your own is kept already and cannot be pinned");
    } else if (err == ESP_ERR_NO_MEM) {
        p3a_http_fail(req, 409, "LIST_IS_FULL", "the list holds %d pins already", P3A_PIN_LIST_MAX);
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        fail_pins(req, err, "pinning");
    } else {
        cJSON *data = net_record_json(&record);
        cJSON_AddBoolToObject(data, "already", err == ESP_ERR_INVALID_STATE);
        p3a_http_ok(req, data);
    }
}

static void post_unpin(p3a_http_request_t *req)
{
    char buf[P3A_PIN_SLUG_LEN + 8];
    const char *slug = NULL;
    if (!slug_param(req, buf, sizeof(buf), &slug)) {
        return;
    }
    const cJSON *position = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "position");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "key");
    p3a_record_t record;
    if (position == NULL && key == NULL) {
        if (!on_screen(req, &record)) {
            return;
        }
    } else {
        if (!cJSON_IsNumber(position) || !cJSON_IsNumber(key) || position->valuedouble < 0 ||
            position->valuedouble >= P3A_PIN_LIST_MAX) {
            p3a_http_fail(req, 400, "INVALID_REQUEST", "an entry is named by \"position\" and \"key\" together");
            return;
        }
        p3a_index_t index = {0};
        esp_err_t err = p3a_index_create(&index, P3A_PIN_LIST_MAX);
        if (err == ESP_OK) {
            err = p3a_pins_load(slug, &index);
        }
        const uint32_t at = (uint32_t)position->valuedouble;
        const bool there = err == ESP_OK && at < index.count && (double)index.records[at].id32 == key->valuedouble;
        if (there) {
            record = index.records[at];
        }
        p3a_index_destroy(&index);
        if (err != ESP_OK) {
            fail_pins(req, err, "reading the list");
            return;
        }
        if (!there) {
            p3a_http_fail(req, 409, "LIST_CHANGED", "that is no longer the pin at that position; read the list again");
            return;
        }
    }
    const esp_err_t err = p3a_pins_unpin(slug, &record);
    if (err == ESP_ERR_NOT_FOUND) {
        p3a_http_fail(req, 409, "NOT_PINNED", "that artwork is not in the list");
    } else {
        err == ESP_OK ? p3a_http_ok(req, NULL) : fail_pins(req, err, "unpinning");
    }
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/pins", get_lists},
    {P3A_HTTP_POST, P3A_HTTP_BODY_JSON, "/api/v2/pins", post_list},
    {P3A_HTTP_GET, 0, "/api/v2/pins/{slug}", get_list},
    {P3A_HTTP_PUT, P3A_HTTP_BODY_JSON, "/api/v2/pins/{slug}", put_list},
    {P3A_HTTP_DELETE, 0, "/api/v2/pins/{slug}", delete_list},
    {P3A_HTTP_POST, 0, "/api/v2/pins/{slug}/pin", post_pin},
    {P3A_HTTP_POST, P3A_HTTP_BODY_JSON, "/api/v2/pins/{slug}/unpin", post_unpin},
};

void net_pins_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
