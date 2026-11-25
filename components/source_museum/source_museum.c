// SPDX-License-Identifier: Apache-2.0
// The museum source on the device: the requests, for every museum. What
// each museum's answers mean is in its own file (museum.h).

#include "source_museum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "museum.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "psram_alloc.h"

static const char *TAG = "museum";

#define URL_BYTES    768
#define PAGE_PAUSE_MS 200  // between two pages: the museums' servers are not built for bursts

static char s_user_agent[48];

static p3a_settings_t *settings_now(void)
{
    p3a_settings_t *settings = psram_malloc(sizeof(*settings));
    if (settings != NULL) {
        p3a_settings_get(settings);
    }
    return settings;
}

static const char *key_of(const museum_t *museum, const p3a_settings_t *settings)
{
    switch (museum->key) {
    case MUSEUM_KEY_HARVARD:
        return settings->harvard_api_key;
    case MUSEUM_KEY_SMITHSONIAN:
        return settings->smithsonian_api_key;
    default:
        return "";
    }
}

static esp_err_t museum_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    const char *axis = NULL;
    const museum_t *museum = museum_of_kind(channel->kind, &axis);
    if (museum == NULL || museum->unavailable != NULL) {
        if (museum != NULL) {
            ESP_LOGW(TAG, "%s cannot be played now: %s", museum->name, museum->unavailable);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (channel->identifier == NULL || channel->identifier[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    p3a_settings_t *settings = settings_now();
    char *body = psram_malloc(museum->response_bytes);
    char *url = psram_malloc(URL_BYTES);
    museum_page_t *page = psram_malloc(sizeof(*page));
    p3a_record_t *records = psram_malloc(museum->page_items * sizeof(p3a_record_t));
    esp_err_t err =
        settings != NULL && body != NULL && url != NULL && page != NULL && records != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && museum->key != MUSEUM_KEY_NONE && key_of(museum, settings)[0] == '\0') {
        err = P3A_ERR_NO_KEY;
    }
    // An offset starts the listing on the page it falls on.
    uint32_t start = museum->offset_cap > 0 ? channel->offset % museum->offset_cap : 0;
    start -= start % museum->page_items;
    uint32_t pages = 0;
    char next[MUSEUM_NEXT_LEN] = "";
    static const http_fetch_header_t headers[] = {{"Accept", "application/json"}};
    while (err == ESP_OK && index->count < index->capacity) {
        const museum_query_t query = {
            .axis = axis, .term = channel->identifier, .api_key = key_of(museum, settings), .start = start, .next = next};
        err = museum->list_url(&query, url, URL_BYTES);
        if (err != ESP_OK) {
            break;
        }
        const http_fetch_request_t req = {.url = url, .user_agent = s_user_agent, .headers = headers, .header_count = 1};
        size_t got = 0;
        err = http_fetch_to_buffer(&req, body, museum->response_bytes, &got, NULL);
        if (err == ESP_OK) {
            memset(page, 0, sizeof(*page));
            page->out = records;
            page->room = museum->page_items;
            page->now = p3a_clock_is_set() ? (uint32_t)time(NULL) : 0;
            err = museum->parse_page(body, page);
        }
        if (err == ESP_OK && page->items == 0 && pages == 0 && start > 0) {
            start = 0;  // the offset is past the end of the channel: from the top
            continue;
        }
        if (err != ESP_OK) {
            if (pages > 0) {
                // The pages already read are a listing; the rest comes with the next one.
                ESP_LOGW(TAG, "%s: a page could not be read (%s); keeping %u records", museum->id, p3a_err_to_name(err),
                         (unsigned)index->count);
                err = ESP_OK;
            }
            break;
        }
        pages++;
        for (uint32_t i = 0; i < page->count && index->count < index->capacity; i++) {
            p3a_index_append(index, &records[i], NULL);  // an artwork listed twice is refused, and fine
        }
        if (!page->more || page->items == 0) {
            break;
        }
        start += page->items;
        memcpy(next, page->next, sizeof(next));
        vTaskDelay(pdMS_TO_TICKS(PAGE_PAUSE_MS));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s %s: %u artworks in %u pages", museum->id, axis, (unsigned)index->count, (unsigned)pages);
    }
    free(records);
    free(page);
    free(url);
    free(body);
    free(settings);
    return err;
}

static esp_err_t museum_url(const p3a_record_t *record, char *out, size_t len)
{
    const museum_t *museum = museum_of_record(record);
    if (museum == NULL) {
        return P3A_ERR_MISSING;  // from a museum this firmware no longer has
    }
    return museum->image_url(record, out, len);
}

static uint32_t museum_refresh_interval_s(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t s = settings != NULL ? (uint32_t)settings->museum_refresh_s : 345600;
    free(settings);
    return s;
}

static uint32_t museum_index_capacity(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t n = settings != NULL ? (uint32_t)settings->museum_cache_size : 1024;
    free(settings);
    return n;
}

esp_err_t source_museum_start(void)
{
    // Museums ask automated clients to say what they are.
    snprintf(s_user_agent, sizeof(s_user_agent), "p3a/%s", esp_app_get_description()->version);
    static const p3a_source_ops_t ops = {
        .source = P3A_SOURCE_MUSEUM,
        .name = "museum",
        .refresh = museum_refresh,
        .remote = true,
        .url = museum_url,
        .user_agent = s_user_agent,
        .redirects = true,  // the museums' image servers send a picture's address on to another
        .refresh_interval_s = museum_refresh_interval_s,
        .index_capacity = museum_index_capacity,
        .smooth = true,         // photographs of paintings: scaled smoothly
        .filter_aspect = true,  // scrolls and friezes are passed over (the aspect_filter setting)
    };
    return p3a_source_register(&ops);
}
