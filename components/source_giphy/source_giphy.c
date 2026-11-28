// SPDX-License-Identifier: Apache-2.0
// The Giphy source on the device: the requests. What the answers mean is
// in giphy_parse.c.

#include "source_giphy.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "giphy_parse.h"
#include "http_fetch.h"
#include "nvs.h"
#include "p3a_board.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_job.h"
#include "p3a_mem.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "psram_alloc.h"

static const char *TAG = "giphy";

#define NVS_NAMESPACE  "p3a_giphy"     // device state: the id Giphy gave this device
#define NVS_RANDOM_ID  "random_id"
#define RESPONSE_BYTES (256 * 1024)    // a page of 50 is about 100 KB
#define URL_BYTES      640
#define RANDOM_ID_LEN  64

static const char *const s_ratings[] = {"g", "pg", "pg-13", "r"};

// The settings are too large for a stack: a copy in PSRAM for the length of a call.
static p3a_settings_t *settings_now(void)
{
    p3a_settings_t *settings = psram_malloc(sizeof(*settings));
    if (settings != NULL) {
        p3a_settings_get(settings);
    }
    return settings;
}

// --- the id Giphy gives a device --------------------------------------------------

static bool random_id_load(char *out, size_t len)
{
    nvs_handle_t nvs;
    out[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    const esp_err_t err = nvs_get_str(nvs, NVS_RANDOM_ID, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && out[0] != '\0';
}

static void random_id_save(const char *id)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_str(nvs, NVS_RANDOM_ID, id) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}

esp_err_t source_giphy_reset_random_id(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, NVS_RANDOM_ID);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

// Asks Giphy for an id. A listing works without one, so failure is only logged.
static void random_id_fetch(const char *api_key, char *body, char *url, char *out, size_t len)
{
    out[0] = '\0';
    if (giphy_random_id_url(api_key, url, URL_BYTES) != ESP_OK) {
        return;
    }
    const http_fetch_request_t req = {.url = url, .user_agent = "p3a", .max_attempts = 1};
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&req, body, 4096, &got, NULL);
    if (err == ESP_OK) {
        err = giphy_parse_random_id(body, out, len);
    }
    if (err == ESP_OK) {
        random_id_save(out);
        ESP_LOGI(TAG, "Giphy gave this device an id");
    } else {
        out[0] = '\0';
        ESP_LOGW(TAG, "no id from Giphy (%s); listing without one", p3a_err_to_name(err));
    }
}

// --- the table ----------------------------------------------------------------------

static esp_err_t giphy_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    const bool search = channel->kind != NULL && strcmp(channel->kind, "search") == 0;
    if (!search && channel->kind != NULL && channel->kind[0] != '\0' && strcmp(channel->kind, "trending") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (search && (channel->identifier == NULL || channel->identifier[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;  // a search for nothing
    }
    p3a_settings_t *settings = settings_now();
    char *body = psram_malloc(RESPONSE_BYTES);
    char *url = psram_malloc(URL_BYTES);
    p3a_record_t *page = psram_malloc(GIPHY_PAGE_ITEMS * sizeof(p3a_record_t));
    esp_err_t err = settings != NULL && body != NULL && url != NULL && page != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && settings->giphy_api_key[0] == '\0') {
        err = P3A_ERR_NO_KEY;
    }
    char random_id[RANDOM_ID_LEN];
    if (err == ESP_OK && !random_id_load(random_id, sizeof(random_id))) {
        random_id_fetch(settings->giphy_api_key, body, url, random_id, sizeof(random_id));
    }
    uint32_t offset = channel->offset % GIPHY_OFFSET_CAP;
    uint32_t pages = 0;
    while (err == ESP_OK && index->count < index->capacity) {
        if (p3a_channel_stopped(channel)) {
            err = ESP_ERR_NOT_FINISHED;  // the playset changed: nobody wants this listing
            break;
        }
        const giphy_list_request_t request = {
            .api_key = settings->giphy_api_key,
            .query = search ? channel->identifier : NULL,
            .rating = s_ratings[settings->giphy_rating < 4 ? settings->giphy_rating : 2],
            .country = settings->giphy_country,
            .random_id = random_id,
            .file = giphy_file_for(settings->giphy_rendition, settings->giphy_format == P3A_GIPHY_FORMAT_WEBP),
            .prefer_downsized = settings->giphy_prefer_downsized,
            .offset = offset,
        };
        err = giphy_list_url(&request, url, URL_BYTES);
        if (err != ESP_OK) {
            break;
        }
        const http_fetch_request_t req = {
            .url = url, .user_agent = "p3a", .should_abort = channel->stop, .user_ctx = channel->stop_ctx};
        size_t got = 0;
        err = http_fetch_to_buffer(&req, body, RESPONSE_BYTES, &got, NULL);
        const giphy_parse_opts_t opts = {
            .file = request.file,
            .prefer_downsized = request.prefer_downsized,
            .screen_w = P3A_DISPLAY_WIDTH,
            .screen_h = P3A_DISPLAY_HEIGHT,
            .now = p3a_clock_is_set() ? (uint32_t)time(NULL) : 0,
        };
        uint32_t count = 0, items = 0;
        if (err == ESP_OK) {
            err = giphy_parse_page(body, &opts, page, GIPHY_PAGE_ITEMS, &count, &items);
        }
        if (err != ESP_OK) {
            if (pages > 0 && !p3a_channel_stopped(channel)) {
                // The pages already read are a listing; the rest comes with the next one.
                ESP_LOGW(TAG, "page at %u could not be read (%s); keeping %u records", (unsigned)offset,
                         p3a_err_to_name(err), (unsigned)index->count);
                err = ESP_OK;
            }
            break;
        }
        if (items == 0 && pages == 0 && offset > 0) {
            // The offset is past the end of what Giphy has for this channel
            // (trending is a couple of hundred GIFs): from the top instead of
            // an empty channel.
            offset = 0;
            continue;
        }
        pages++;
        for (uint32_t i = 0; i < count && index->count < index->capacity; i++) {
            p3a_index_append(index, &page[i], NULL);  // Giphy repeats a GIF across pages now and then: refused, and fine
        }
        offset += items;
        if (items < GIPHY_PAGE_ITEMS || offset >= GIPHY_OFFSET_CAP) {
            break;
        }
        vTaskDelay(1);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s%s%s: %u GIFs in %u pages", search ? "search \"" : "trending", search ? channel->identifier : "",
                 search ? "\"" : "", (unsigned)index->count, (unsigned)pages);
    }
    free(page);
    free(url);
    free(body);
    free(settings);
    return err;
}

// --- a click ------------------------------------------------------------------------
//
// Giphy asks the makers of players to tell it when a person acts on a GIF.
// A swipe up is that act. Two small requests, on the I/O runner; nobody
// waits for them and a failure is only logged.

static esp_err_t click_job(void *arg)
{
    p3a_record_t *record = arg;
    p3a_settings_t *settings = settings_now();
    char *body = psram_malloc(4096);
    char *url = psram_malloc(URL_BYTES);
    char random_id[RANDOM_ID_LEN];
    esp_err_t err = settings != NULL && body != NULL && url != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && (settings->giphy_api_key[0] == '\0' || !random_id_load(random_id, sizeof(random_id)))) {
        err = ESP_ERR_INVALID_STATE;  // no key, or Giphy never gave this device an id: nothing to report with
    }
    if (err == ESP_OK && p3a_mem_level() != P3A_MEM_OK) {
        err = ESP_ERR_NO_MEM;  // optional work
    }
    if (err == ESP_OK) {
        err = giphy_click_lookup_url(settings->giphy_api_key, record, random_id, url, URL_BYTES);
    }
    if (err == ESP_OK) {
        const http_fetch_request_t req = {.url = url, .user_agent = "p3a", .max_attempts = 1};
        size_t got = 0;
        err = http_fetch_to_buffer(&req, body, 4096, &got, NULL);
    }
    if (err == ESP_OK) {
        const uint64_t now_ms = p3a_clock_is_set() ? (uint64_t)time(NULL) * 1000 : 0;
        err = giphy_parse_click_url(body, random_id, now_ms, url, URL_BYTES);
    }
    if (err == ESP_OK) {
        const http_fetch_request_t req = {.url = url, .user_agent = "p3a", .max_attempts = 1};
        size_t got = 0;
        err = http_fetch_to_buffer(&req, body, 4096, &got, NULL);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "click on %s told to Giphy", record->identity.giphy.id);
    } else {
        ESP_LOGW(TAG, "click on %s not told to Giphy: %s", record->identity.giphy.id, p3a_err_to_name(err));
    }
    free(url);
    free(body);
    free(settings);
    free(record);
    return err;
}

static p3a_reaction_t giphy_react(const p3a_record_t *record, bool undo)
{
    if (undo) {
        return P3A_REACTION_NONE;  // a click is not taken back
    }
    p3a_record_t *copy = psram_malloc(sizeof(*copy));
    if (copy == NULL) {
        return P3A_REACTION_NONE;
    }
    *copy = *record;
    if (p3a_job_submit(P3A_JOBS_IO, "giphy click", click_job, copy, NULL) != ESP_OK) {
        free(copy);
        return P3A_REACTION_NONE;
    }
    return P3A_REACTION_NOTED;
}

static esp_err_t giphy_url(const p3a_record_t *record, char *out, size_t len)
{
    return giphy_download_url(record, out, len);
}

static uint32_t giphy_refresh_interval_s(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t s = settings != NULL ? (uint32_t)settings->giphy_refresh_s : 7200;
    free(settings);
    return s;
}

static uint32_t giphy_index_capacity(void)
{
    p3a_settings_t *settings = settings_now();
    const uint32_t n = settings != NULL ? (uint32_t)settings->giphy_cache_size : 350;
    free(settings);
    return n;
}

// What one channel of a playset may set for itself: the ranges of the two settings above.
static void giphy_channel_range(p3a_channel_range_t *out)
{
    out->capacity_min = (uint32_t)p3a_setting_defs[P3A_SET_GIPHY_CACHE_SIZE].min;
    out->capacity_max = (uint32_t)p3a_setting_defs[P3A_SET_GIPHY_CACHE_SIZE].max;
    out->refresh_min_s = (uint32_t)p3a_setting_defs[P3A_SET_GIPHY_REFRESH_S].min;
    out->refresh_max_s = (uint32_t)p3a_setting_defs[P3A_SET_GIPHY_REFRESH_S].max;
}

esp_err_t source_giphy_start(void)
{
    // The service's own marks (credit/README.md).
    extern const uint8_t dark_start[] asm("_binary_powered_by_giphy_on_dark_p3aimg_start");
    extern const uint8_t dark_end[] asm("_binary_powered_by_giphy_on_dark_p3aimg_end");
    extern const uint8_t light_start[] asm("_binary_powered_by_giphy_on_light_p3aimg_start");
    extern const uint8_t light_end[] asm("_binary_powered_by_giphy_on_light_p3aimg_end");
    static const p3a_source_ops_t ops = {
        .source = P3A_SOURCE_GIPHY,
        .name = "giphy",
        .refresh = giphy_refresh,
        .remote = true,
        .url = giphy_url,
        .react = giphy_react,
        .credit = dark_start,
        .credit_end = dark_end,
        .credit_light = light_start,
        .credit_light_end = light_end,
        .refresh_interval_s = giphy_refresh_interval_s,
        .index_capacity = giphy_index_capacity,
        .channel_range = giphy_channel_range,
        .smooth = true,  // photographs and video, mostly: scaled smoothly
    };
    return p3a_source_register(&ops);
}
