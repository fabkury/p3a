// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file giphy_api.c
 * @brief Giphy API client - fetches trending GIFs via HTTP
 */

#include "giphy.h"
#include "giphy_types.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "giphy_api";

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

esp_err_t giphy_fetch_trending_page(giphy_fetch_ctx_t *ctx, int offset,
                                    giphy_channel_entry_t *out_entries,
                                    size_t *out_count, bool *out_has_more)
{
    if (!ctx || !out_entries || !out_count || !out_has_more) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    *out_has_more = false;

    // Build URL
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.giphy.com/v1/gifs/trending?api_key=%s&limit=%d&offset=%d&rating=%s",
             ctx->api_key, GIPHY_PAGE_LIMIT, offset, ctx->rating);

    ESP_LOGI(TAG, "Fetching trending: offset=%d, limit=%d", offset, GIPHY_PAGE_LIMIT);

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
        return ESP_ERR_NO_MEM;
    }

    // Perform request
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Giphy API returned status %d", status);
        char err_buf[256];
        int err_read = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (err_read > 0) {
            err_buf[err_read] = '\0';
            ESP_LOGW(TAG, "DEBUG: Error response body: %s", err_buf);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (status == 401 || status == 403) return ESP_ERR_NOT_ALLOWED;
        if (status == 429) return ESP_ERR_INVALID_RESPONSE;
        return ESP_FAIL;
    }

    // Read response body
    int total_read = 0;
    int read_len;
    while (total_read < (int)ctx->response_buf_size - 1) {
        read_len = esp_http_client_read(client, ctx->response_buf + total_read,
                                        ctx->response_buf_size - 1 - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    ctx->response_buf[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Received %d bytes from Giphy API", total_read);

    if (total_read > 0 && total_read < 150) {
        ESP_LOGW(TAG, "DEBUG: Small response (%d bytes), full body: %s", total_read, ctx->response_buf);
    }

    if (total_read == 0) {
        ESP_LOGE(TAG, "Empty response from Giphy API");
        return ESP_FAIL;
    }

    if (total_read >= (int)ctx->response_buf_size - 1) {
        ESP_LOGE(TAG, "Response truncated at %d bytes (buffer full)", total_read);
        return ESP_FAIL;
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(ctx->response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse Giphy JSON response (%d bytes)", total_read);
        char snippet[201];
        int snip_len = total_read < 200 ? total_read : 200;
        memcpy(snippet, ctx->response_buf, snip_len);
        snippet[snip_len] = '\0';
        ESP_LOGE(TAG, "Response start: %.200s", snippet);
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "Giphy response missing 'data' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int array_size = cJSON_GetArraySize(data);
    if (array_size == 0) {
        ESP_LOGI(TAG, "No more trending results at offset %d", offset);
        cJSON_Delete(root);
        return ESP_OK;  // Success, but 0 entries — caller sees *out_count == 0
    }

    // Parse each GIF object
    size_t parsed = 0;
    for (int i = 0; i < array_size; i++) {
        const cJSON *gif = cJSON_GetArrayItem(data, i);
        if (parse_gif_object(gif, &out_entries[parsed], ctx->rendition, ctx->format)) {
            parsed++;
        }
    }

    ESP_LOGI(TAG, "Parsed %zu/%d GIFs at offset %d", parsed, array_size, offset);

    cJSON_Delete(root);

    *out_count = parsed;
    *out_has_more = (array_size >= GIPHY_PAGE_LIMIT && (offset + array_size) < 499);

    return ESP_OK;
}
