// SPDX-License-Identifier: Apache-2.0
// The Klipy source on the device: the requests. What the answers mean is
// in klipy_parse.c.

#include "source_klipy.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "klipy_parse.h"
#include "p3a_board.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "psram_alloc.h"

static const char *TAG = "klipy";

#define RESPONSE_BYTES (512 * 1024)  // a page of 50 is about 250 KB: every item lists twenty files
#define ITEM_BYTES     (32 * 1024)
#define URL_BYTES      640

static const char *const s_ratings[] = {"g", "pg", "pg-13", "r"};

static p3a_settings_t *settings_now(void)
{
    p3a_settings_t *settings = psram_malloc(sizeof(*settings));
    if (settings != NULL) {
        p3a_settings_get(settings);
    }
    return settings;
}

static klipy_parse_opts_t opts_for(const p3a_settings_t *settings, uint8_t product)
{
    return (klipy_parse_opts_t){
        .product = product,
        .webp = settings->klipy_format == P3A_KLIPY_FORMAT_WEBP,
        .screen_w = P3A_DISPLAY_WIDTH,
        .screen_h = P3A_DISPLAY_HEIGHT,
        .now = p3a_clock_is_set() ? (uint32_t)time(NULL) : 0,
    };
}

// Klipy answers a key it does not know with 404, on a route that exists.
static esp_err_t key_refused(esp_err_t err)
{
    return err == (esp_err_t)P3A_ERR_MISSING ? (esp_err_t)P3A_ERR_AUTH : err;
}

static esp_err_t klipy_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    uint8_t product;
    klipy_mode_t mode;
    if (!klipy_kind_parse(channel->kind, &product, &mode)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const bool search = mode == KLIPY_MODE_SEARCH;
    if (search && (channel->identifier == NULL || channel->identifier[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;  // a search for nothing
    }
    p3a_settings_t *settings = settings_now();
    char *body = psram_malloc(RESPONSE_BYTES);
    char *url = psram_malloc(URL_BYTES);
    p3a_record_t *page_records = psram_malloc(KLIPY_PAGE_ITEMS * sizeof(p3a_record_t));
    esp_err_t err = settings != NULL && body != NULL && url != NULL && page_records != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && settings->klipy_api_key[0] == '\0') {
        err = P3A_ERR_NO_KEY;
    }
    // Pages are numbered from 1; an offset starts at the page it falls on.
    uint32_t page = (channel->offset % KLIPY_OFFSET_CAP) / KLIPY_PAGE_ITEMS + 1;
    uint32_t pages = 0;
    while (err == ESP_OK && index->count < index->capacity) {
        if (p3a_channel_stopped(channel)) {
            err = ESP_ERR_NOT_FINISHED;  // the playset changed: nobody wants this listing
            break;
        }
        err = klipy_list_url(settings->klipy_api_key, product, search ? channel->identifier : NULL, page,
                             s_ratings[settings->klipy_rating < 4 ? settings->klipy_rating : 2], url, URL_BYTES);
        if (err != ESP_OK) {
            break;
        }
        const http_fetch_request_t req = {
            .url = url, .user_agent = "p3a", .should_abort = channel->stop, .user_ctx = channel->stop_ctx};
        const klipy_parse_opts_t opts = opts_for(settings, product);
        size_t got = 0;
        uint32_t count = 0, items = 0;
        bool has_next = false;
        err = key_refused(http_fetch_to_buffer(&req, body, RESPONSE_BYTES, &got, NULL));
        if (err == ESP_OK) {
            err = klipy_parse_page(body, &opts, page_records, KLIPY_PAGE_ITEMS, &count, &items, &has_next);
        }
        if (err != ESP_OK) {
            if (pages > 0 && !p3a_channel_stopped(channel)) {
                ESP_LOGW(TAG, "page %u could not be read (%s); keeping %u records", (unsigned)page, p3a_err_to_name(err),
                         (unsigned)index->count);
                err = ESP_OK;
            }
            break;
        }
        if (items == 0 && pages == 0 && page > 1) {
            page = 1;  // the offset is past the end of the channel: from the top
            continue;
        }
        pages++;
        for (uint32_t i = 0; i < count && index->count < index->capacity; i++) {
            p3a_index_append(index, &page_records[i], NULL);  // an item listed twice is refused, and fine
        }
        if (!has_next || items == 0) {
            break;
        }
        page++;
        vTaskDelay(1);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s %s%s%s: %u items in %u pages", product == KLIPY_PRODUCT_STICKER ? "stickers" : "GIFs",
                 search ? "\"" : "trending", search ? channel->identifier : "", search ? "\"" : "",
                 (unsigned)index->count, (unsigned)pages);
    }
    free(page_records);
    free(url);
    free(body);
    free(settings);
    return err;
}

// Where an item's bytes are: Klipy is asked, because the address cannot be
// worked out from the record.
static esp_err_t klipy_url(const p3a_record_t *record, char *out, size_t len)
{
    p3a_settings_t *settings = settings_now();
    char *body = psram_malloc(ITEM_BYTES);
    char *url = psram_malloc(URL_BYTES);
    esp_err_t err = settings != NULL && body != NULL && url != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && settings->klipy_api_key[0] == '\0') {
        err = P3A_ERR_NO_KEY;
    }
    if (err == ESP_OK) {
        err = klipy_item_url(settings->klipy_api_key, record, url, URL_BYTES);
    }
    if (err == ESP_OK) {
        const http_fetch_request_t req = {.url = url, .user_agent = "p3a"};
        size_t got = 0;
        err = key_refused(http_fetch_to_buffer(&req, body, ITEM_BYTES, &got, NULL));
    }
    if (err == ESP_OK) {
        const klipy_parse_opts_t opts = opts_for(settings, record->identity.klipy.product);
        err = klipy_parse_item_url(body, &opts, out, len);
    }
    free(url);
    free(body);
    free(settings);
    return err;
}

static uint32_t klipy_refresh_interval_s(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t s = settings != NULL ? (uint32_t)settings->klipy_refresh_s : 7200;
    free(settings);
    return s;
}

static uint32_t klipy_index_capacity(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t n = settings != NULL ? (uint32_t)settings->klipy_cache_size : 350;
    free(settings);
    return n;
}

// What one channel of a playset may set for itself: the ranges of the two settings above.
static void klipy_channel_range(p3a_channel_range_t *out)
{
    out->capacity_min = (uint32_t)p3a_setting_defs[P3A_SET_KLIPY_CACHE_SIZE].min;
    out->capacity_max = (uint32_t)p3a_setting_defs[P3A_SET_KLIPY_CACHE_SIZE].max;
    out->refresh_min_s = (uint32_t)p3a_setting_defs[P3A_SET_KLIPY_REFRESH_S].min;
    out->refresh_max_s = (uint32_t)p3a_setting_defs[P3A_SET_KLIPY_REFRESH_S].max;
}

esp_err_t source_klipy_start(void)
{
    // The service's own marks (credit/README.md).
    extern const uint8_t dark_start[] asm("_binary_powered_by_klipy_on_dark_p3aimg_start");
    extern const uint8_t dark_end[] asm("_binary_powered_by_klipy_on_dark_p3aimg_end");
    extern const uint8_t light_start[] asm("_binary_powered_by_klipy_on_light_p3aimg_start");
    extern const uint8_t light_end[] asm("_binary_powered_by_klipy_on_light_p3aimg_end");
    static const p3a_source_ops_t ops = {
        .source = P3A_SOURCE_KLIPY,
        .name = "klipy",
        .refresh = klipy_refresh,
        .remote = true,
        .url = klipy_url,
        .credit = dark_start,
        .credit_end = dark_end,
        .credit_light = light_start,
        .credit_light_end = light_end,
        .refresh_interval_s = klipy_refresh_interval_s,
        .index_capacity = klipy_index_capacity,
        .channel_range = klipy_channel_range,
        .smooth = true,
    };
    return p3a_source_register(&ops);
}
