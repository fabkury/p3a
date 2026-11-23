// SPDX-License-Identifier: Apache-2.0
// A source for tests: a remote source whose listing, addresses and failures
// the test sets. It lets the content task (listing, the index on the card,
// downloads, back-off) be tested without depending on what a real service
// lists today. Registered as the channel source "test", which a release
// build does not have.
//
// !content set {"items": [{"id": "a", "url": "https://..."}, ...],
//               "result": "ok" | "RATE_LIMITED" | "NO_KEY" | "AUTH" | "FAILED",
//               "interval_s": 3600}
//     What the source lists from now on (at most 12 items) and how a listing
//     ends. Until the first `set` after a boot a listing fails, as a
//     service that cannot be reached would. An item with no "url" is one
//     the source no longer has.
// !content listed    how many times the source was asked for its listing since boot
//
// Its records are Giphy records (an id is all they need).

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_err.h"
#include "p3a_source.h"
#include "p3a_test_hooks.h"
#include "p3a_test_internal.h"

#define ITEMS_MAX 12

typedef struct {
    char id[P3A_RECORD_GIPHY_ID_LEN];
    char url[200];
} item_t;

static item_t s_items[ITEMS_MAX];
static uint32_t s_count;
static esp_err_t s_result = ESP_FAIL;
static uint32_t s_interval_s = 3600;
static atomic_uint s_listed;
static SemaphoreHandle_t s_lock;

static esp_err_t refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    (void)channel;
    atomic_fetch_add(&s_listed, 1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_result;
    for (uint32_t i = 0; err == ESP_OK && i < s_count; i++) {
        p3a_record_t record;
        err = p3a_record_init_giphy(&record, s_items[i].id);
        if (err == ESP_OK) {
            err = p3a_index_append(index, &record, NULL);
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t url(const p3a_record_t *record, char *out, size_t len)
{
    esp_err_t err = P3A_ERR_MISSING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < s_count; i++) {
        if (strcmp(s_items[i].id, record->identity.giphy.id) == 0 && s_items[i].url[0] != '\0') {
            err = snprintf(out, len, "%s", s_items[i].url) < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static uint32_t interval(void)
{
    return s_interval_s;
}

static uint32_t capacity(void)
{
    return 32;
}

static const p3a_source_ops_t s_ops = {
    .source = P3A_CHANNEL_SOURCE_TEST,
    .name = "test",
    .refresh = refresh,
    .remote = true,
    .url = url,
    .user_agent = "p3a-test",
    .refresh_interval_s = interval,
    .index_capacity = capacity,
    .smooth = true,
};

void p3a_test_sources_register(void)
{
    s_lock = xSemaphoreCreateMutex();
    p3a_source_register(&s_ops);
}

static esp_err_t result_of(const char *name)
{
    static const struct {
        const char *name;
        esp_err_t err;
    } known[] = {{"ok", ESP_OK},
                 {"RATE_LIMITED", P3A_ERR_RATE_LIMITED},
                 {"NO_KEY", P3A_ERR_NO_KEY},
                 {"AUTH", P3A_ERR_AUTH},
                 {"FAILED", ESP_FAIL}};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(name, known[i].name) == 0) {
            return known[i].err;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static void cmd_content(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "listed", 6) == 0) {
        char text[40];
        snprintf(text, sizeof(text), "{\"listed\":%u}", (unsigned)atomic_load(&s_listed));
        p3a_test_reply_ok(text);
        return;
    }
    cJSON *json = strncmp(args, "set ", 4) == 0 ? cJSON_Parse(args + 4) : NULL;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(json, "result");
    const cJSON *interval_s = cJSON_GetObjectItemCaseSensitive(json, "interval_s");
    const esp_err_t wanted = cJSON_IsString(result) ? result_of(result->valuestring) : ESP_OK;
    if (json == NULL || wanted == ESP_ERR_INVALID_ARG || (items != NULL && cJSON_GetArraySize(items) > ITEMS_MAX)) {
        cJSON_Delete(json);
        p3a_test_reply_err("content set {\"items\":[{\"id\":..,\"url\":..}],\"result\":..,\"interval_s\":n} | content listed");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_result = wanted;
    if (cJSON_IsNumber(interval_s)) {
        s_interval_s = (uint32_t)interval_s->valueint;
    }
    if (items != NULL) {
        s_count = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, items) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
            const cJSON *address = cJSON_GetObjectItemCaseSensitive(item, "url");
            if (!cJSON_IsString(id)) {
                continue;
            }
            item_t *it = &s_items[s_count++];
            snprintf(it->id, sizeof(it->id), "%s", id->valuestring);
            snprintf(it->url, sizeof(it->url), "%s", cJSON_IsString(address) ? address->valuestring : "");
        }
    }
    const uint32_t count = s_count;
    xSemaphoreGive(s_lock);
    cJSON_Delete(json);
    char text[40];
    snprintf(text, sizeof(text), "{\"items\":%u}", (unsigned)count);
    p3a_test_reply_ok(text);
}

const p3a_test_command_t p3a_test_content_commands[] = {
    {"content", cmd_content},
};
const size_t p3a_test_content_command_count =
    sizeof(p3a_test_content_commands) / sizeof(p3a_test_content_commands[0]);
