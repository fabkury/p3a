// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file giphy_api.c
 * @brief Giphy API client - fetches trending GIFs via HTTP
 */

#include "giphy.h"
#include "giphy_types.h"
#include "config_store.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "giphy_api";

// Maximum response buffer size (allocated in PSRAM).
// Each GIF object is ~8KB of JSON (20+ renditions with full URLs).
// At 25 items/page this is ~200KB, so 256KB provides safe headroom.
#define GIPHY_RESPONSE_BUF_SIZE (256 * 1024)

// Maximum items per API call.
// Giphy beta key allows up to 50, but each GIF object is ~8KB of JSON,
// so 50 items produces ~400KB responses that exceed reasonable buffers.
// 25 (Giphy's own default) keeps responses under ~200KB.
#define GIPHY_PAGE_LIMIT 25

/**
 * @brief Parse a single GIF object from the Giphy API response
 *
 * Extracts id, dimensions, timestamps, and fills a giphy_channel_entry_t.
 */
static bool parse_gif_object(const cJSON *gif, giphy_channel_entry_t *out_entry,
                             const char *rendition_name, const char *format_name)
{
    if (!gif || !out_entry) return false;

    // Extract GIF id
    const cJSON *id_json = cJSON_GetObjectItem(gif, "id");
    if (!cJSON_IsString(id_json) || !id_json->valuestring[0]) return false;

    const char *gif_id = id_json->valuestring;
    size_t id_len = strlen(gif_id);
    if (id_len >= sizeof(out_entry->giphy_id)) {
        ESP_LOGW(TAG, "GIF id too long (%zu chars): %.20s...", id_len, gif_id);
        return false;
    }

    memset(out_entry, 0, sizeof(*out_entry));

    // Store giphy_id
    strlcpy(out_entry->giphy_id, gif_id, sizeof(out_entry->giphy_id));

    // Compute post_id
    out_entry->post_id = giphy_id_to_post_id(gif_id);
    out_entry->kind = 0;  // Artwork

    // Determine extension from format
    if (strcmp(format_name, "gif") == 0) {
        out_entry->extension = 1;  // gif
    } else {
        out_entry->extension = 0;  // webp (default)
    }

    // Extract dimensions from the configured rendition
    const cJSON *images = cJSON_GetObjectItem(gif, "images");
    if (cJSON_IsObject(images)) {
        const cJSON *rendition = cJSON_GetObjectItem(images, rendition_name);
        if (cJSON_IsObject(rendition)) {
            const cJSON *w = cJSON_GetObjectItem(rendition, "width");
            const cJSON *h = cJSON_GetObjectItem(rendition, "height");
            if (cJSON_IsString(w)) out_entry->width = (uint16_t)atoi(w->valuestring);
            if (cJSON_IsString(h)) out_entry->height = (uint16_t)atoi(h->valuestring);
        }
    }

    // Extract timestamp (prefer trending_datetime, fall back to import_datetime)
    const cJSON *trending_dt = cJSON_GetObjectItem(gif, "trending_datetime");
    const cJSON *import_dt = cJSON_GetObjectItem(gif, "import_datetime");
    const char *dt_str = NULL;
    if (cJSON_IsString(trending_dt) && trending_dt->valuestring[0] &&
        strcmp(trending_dt->valuestring, "0000-00-00 00:00:00") != 0) {
        dt_str = trending_dt->valuestring;
    } else if (cJSON_IsString(import_dt) && import_dt->valuestring[0]) {
        dt_str = import_dt->valuestring;
    }

    if (dt_str) {
        // Parse "YYYY-MM-DD HH:MM:SS" format
        struct tm tm = {0};
        if (sscanf(dt_str, "%d-%d-%d %d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            out_entry->created_at = (uint32_t)mktime(&tm);
        }
    }

    if (out_entry->created_at == 0) {
        out_entry->created_at = (uint32_t)time(NULL);
    }

    return true;
}

esp_err_t giphy_fetch_trending(giphy_channel_entry_t *out_entries,
                               size_t max_entries, size_t *out_count)
{
    if (!out_entries || !out_count || max_entries == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;

    // Get config values
    char api_key[128];
    config_store_get_giphy_api_key(api_key, sizeof(api_key));
    if (api_key[0] == '\0') {
        ESP_LOGE(TAG, "No Giphy API key configured");
        return ESP_ERR_NOT_FOUND;
    }

    char rendition[32];
    if (config_store_get_giphy_rendition(rendition, sizeof(rendition)) != ESP_OK) {
        strlcpy(rendition, CONFIG_GIPHY_RENDITION_DEFAULT, sizeof(rendition));
    }

    char format[8];
    if (config_store_get_giphy_format(format, sizeof(format)) != ESP_OK) {
        strlcpy(format, CONFIG_GIPHY_FORMAT_DEFAULT, sizeof(format));
    }

    char rating[8];
    if (config_store_get_giphy_rating(rating, sizeof(rating)) != ESP_OK) {
        strlcpy(rating, "pg-13", sizeof(rating));
    }

    // Allocate response buffer in PSRAM
    char *response_buf = heap_caps_malloc(GIPHY_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(GIPHY_RESPONSE_BUF_SIZE);
        if (!response_buf) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total_fetched = 0;
    int offset = 0;

    while (total_fetched < max_entries) {
        if (giphy_is_refresh_cancelled()) {
            ESP_LOGI(TAG, "Fetch cancelled before next batch (offset=%d)", offset);
            break;
        }

        int page_limit = GIPHY_PAGE_LIMIT;
        if (total_fetched + page_limit > max_entries) {
            page_limit = (int)(max_entries - total_fetched);
        }

        // Build URL
        char url[512];
        snprintf(url, sizeof(url),
                 "https://api.giphy.com/v1/gifs/trending?api_key=%s&limit=%d&offset=%d&rating=%s",
                 api_key, page_limit, offset, rating);

        ESP_LOGI(TAG, "Fetching trending: offset=%d, limit=%d", offset, page_limit);

        // Configure HTTP client
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }

        // Perform request
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            free(response_buf);
            return err;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status != 200) {
            ESP_LOGE(TAG, "Giphy API returned status %d", status);
            // Debug: read and log error response body
            char err_buf[256];
            int err_read = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
            if (err_read > 0) {
                err_buf[err_read] = '\0';
                ESP_LOGW(TAG, "DEBUG: Error response body: %s", err_buf);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(response_buf);
            if (status == 401 || status == 403) return ESP_ERR_NOT_ALLOWED;
            if (status == 429) return ESP_ERR_INVALID_RESPONSE;
            return ESP_FAIL;
        }

        // Read response body
        int total_read = 0;
        int read_len;
        while (total_read < GIPHY_RESPONSE_BUF_SIZE - 1) {
            read_len = esp_http_client_read(client, response_buf + total_read,
                                            GIPHY_RESPONSE_BUF_SIZE - 1 - total_read);
            if (read_len <= 0) break;
            total_read += read_len;
        }
        response_buf[total_read] = '\0';

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (giphy_is_refresh_cancelled()) {
            ESP_LOGI(TAG, "Fetch cancelled after HTTP response (offset=%d)", offset);
            break;
        }

        ESP_LOGI(TAG, "Received %d bytes from Giphy API", total_read);

        if (total_read > 0 && total_read < 150) {
            ESP_LOGW(TAG, "DEBUG: Small response (%d bytes), full body: %s", total_read, response_buf);
        }

        if (total_read == 0) {
            ESP_LOGE(TAG, "Empty response from Giphy API");
            break;
        }

        if (total_read >= GIPHY_RESPONSE_BUF_SIZE - 1) {
            ESP_LOGE(TAG, "Response truncated at %d bytes (buffer full) - reduce page limit", total_read);
            break;
        }

        // Parse JSON
        cJSON *root = cJSON_Parse(response_buf);
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse Giphy JSON response (%d bytes)", total_read);
            // Log first 200 chars for debugging
            char snippet[201];
            int snip_len = total_read < 200 ? total_read : 200;
            memcpy(snippet, response_buf, snip_len);
            snippet[snip_len] = '\0';
            ESP_LOGE(TAG, "Response start: %.200s", snippet);
            break;
        }

        const cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsArray(data)) {
            ESP_LOGE(TAG, "Giphy response missing 'data' array");
            cJSON_Delete(root);
            break;
        }

        int array_size = cJSON_GetArraySize(data);
        if (array_size == 0) {
            ESP_LOGI(TAG, "No more trending results at offset %d", offset);

            // Debug: log meta object if present
            const cJSON *meta = cJSON_GetObjectItem(root, "meta");
            if (cJSON_IsObject(meta)) {
                const cJSON *meta_status = cJSON_GetObjectItem(meta, "status");
                const cJSON *meta_msg = cJSON_GetObjectItem(meta, "msg");
                ESP_LOGW(TAG, "DEBUG: meta.status=%d, meta.msg=%s",
                         cJSON_IsNumber(meta_status) ? meta_status->valueint : -1,
                         cJSON_IsString(meta_msg) ? meta_msg->valuestring : "(none)");
            }

            cJSON_Delete(root);
            break;
        }

        // Parse each GIF object
        int page_parsed = 0;
        for (int i = 0; i < array_size && total_fetched < max_entries; i++) {
            const cJSON *gif = cJSON_GetArrayItem(data, i);
            if (parse_gif_object(gif, &out_entries[total_fetched], rendition, format)) {
                total_fetched++;
                page_parsed++;
            }
        }

        ESP_LOGI(TAG, "Parsed %d/%d GIFs (total: %zu/%zu)",
                 page_parsed, array_size, total_fetched, max_entries);

        // Check pagination
        const cJSON *pagination = cJSON_GetObjectItem(root, "pagination");
        if (cJSON_IsObject(pagination)) {
            const cJSON *tc = cJSON_GetObjectItem(pagination, "total_count");
            (void)tc;  // total_count available for future use
        }

        cJSON_Delete(root);

        // Move to next page
        offset += array_size;

        // Stop if we've reached the end of results or API offset limit
        if (array_size < page_limit || offset >= 499) {
            break;
        }

        // Brief delay between pages to be nice to the API
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    *out_count = total_fetched;

    ESP_LOGI(TAG, "Trending fetch complete: %zu entries", total_fetched);
    return (total_fetched > 0) ? ESP_OK : ESP_FAIL;
}
