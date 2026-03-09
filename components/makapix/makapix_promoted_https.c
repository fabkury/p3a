// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_promoted_https.c
 * @brief HTTPS-based refresh for the promoted channel
 *
 * Provides a fallback refresh path for devices without Makapix Club
 * registration (no MQTT). Fetches promoted artworks from the public
 * REST API and merges them into the same channel cache used by the
 * MQTT path.
 */

#include "makapix_promoted_https.h"
#include "makapix_api.h"        // For makapix_post_t, MAKAPIX_MAX_POSTS_PER_RESPONSE
#include "channel_cache.h"      // For channel_cache_registry_find, channel_cache_merge_posts
#include "channel_metadata.h"   // For channel_metadata_save
#include "config_store.h"       // For config_store_get_channel_cache_size, config_store_get_refresh_interval_sec
#include "download_manager.h"   // For download_manager_rescan
#include "sd_path.h"            // For sd_path_get_channel, sd_path_get_vault
#include "sntp_sync.h"          // For sntp_sync_is_synchronized
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// Forward declaration to avoid pulling in play_scheduler.h (which has heavy deps)
void ps_get_display_name(const char *channel_id, char *out, size_t out_len);

static const char *TAG = "mkx_promoted_https";

// Response buffer size (PSRAM preferred)
#define RESPONSE_BUF_SIZE (128 * 1024)

// Maximum entries per page (matches Makapix API limit)
#define PAGE_LIMIT 50

// Pagination cursor buffer size (base64 cursors can be ~76+ chars)
#define CURSOR_BUF_SIZE 256

static volatile bool s_cancel = false;

void makapix_promoted_https_cancel(void)
{
    s_cancel = true;
}

bool makapix_promoted_https_is_cancelled(void)
{
    return s_cancel;
}

/**
 * @brief Parse a single item from the promoted feed JSON into a makapix_post_t
 */
static bool parse_promoted_item(const cJSON *item, makapix_post_t *out)
{
    if (!item || !out) return false;

    memset(out, 0, sizeof(*out));

    const cJSON *id_json = cJSON_GetObjectItem(item, "id");
    if (!cJSON_IsNumber(id_json)) return false;
    out->post_id = id_json->valueint;

    out->kind = MAKAPIX_POST_KIND_ARTWORK;

    const cJSON *sk = cJSON_GetObjectItem(item, "storage_key");
    if (cJSON_IsString(sk) && sk->valuestring[0]) {
        strlcpy(out->storage_key, sk->valuestring, sizeof(out->storage_key));
    } else {
        return false;  // storage_key is required
    }

    // Synthetic art_url for detect_file_type() — download manager builds the
    // real URL from storage_key_uuid + extension
    strlcpy(out->art_url, ".webp", sizeof(out->art_url));

    const cJSON *ca = cJSON_GetObjectItem(item, "created_at");
    if (cJSON_IsString(ca) && ca->valuestring) {
        strlcpy(out->created_at, ca->valuestring, sizeof(out->created_at));
    }

    const cJSON *ama = cJSON_GetObjectItem(item, "artwork_modified_at");
    if (cJSON_IsString(ama) && ama->valuestring) {
        strlcpy(out->artwork_modified_at, ama->valuestring, sizeof(out->artwork_modified_at));
    }

    return true;
}

/**
 * @brief Fetch one page from the promoted feed API
 *
 * @param response_buf   Pre-allocated buffer for HTTP response body
 * @param buf_size       Size of response_buf
 * @param cursor         Pagination cursor (NULL or empty for first page)
 * @param out_posts      Output array (caller-provided, PAGE_LIMIT capacity)
 * @param out_count      Number of posts parsed
 * @param out_next_cursor Output buffer for next_cursor (CURSOR_BUF_SIZE bytes min)
 * @param out_has_more   Whether more pages are available
 * @return ESP_OK on success
 */
static esp_err_t fetch_promoted_page(char *response_buf, size_t buf_size,
                                     const char *cursor,
                                     makapix_post_t *out_posts, size_t *out_count,
                                     char *out_next_cursor, bool *out_has_more)
{
    *out_count = 0;
    *out_has_more = false;
    out_next_cursor[0] = '\0';

    // Build URL
    char url[512];
    if (cursor && cursor[0] != '\0') {
        snprintf(url, sizeof(url),
                 "https://%s/api/feed/promoted"
                 "?fields=id,storage_key,created_at,artwork_modified_at"
                 "&limit=%d&cursor=%s",
                 CONFIG_MAKAPIX_CLUB_HOST, PAGE_LIMIT, cursor);
    } else {
        snprintf(url, sizeof(url),
                 "https://%s/api/feed/promoted"
                 "?fields=id,storage_key,created_at,artwork_modified_at"
                 "&limit=%d",
                 CONFIG_MAKAPIX_CLUB_HOST, PAGE_LIMIT);
    }

    ESP_LOGI(TAG, "Fetching promoted page%s%s",
             (cursor && cursor[0]) ? " cursor=" : "",
             (cursor && cursor[0]) ? cursor : "");

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

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Promoted API returned status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Read response body
    int total_read = 0;
    int read_len;
    while (total_read < (int)buf_size - 1) {
        read_len = esp_http_client_read(client, response_buf + total_read,
                                        buf_size - 1 - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    response_buf[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGE(TAG, "Empty response from promoted API");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Received %d bytes from promoted API", total_read);

    // Parse JSON
    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse promoted JSON (%d bytes)", total_read);
        return ESP_FAIL;
    }

    // Parse items array
    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "No 'items' array in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (count >= PAGE_LIMIT) break;
        if (parse_promoted_item(item, &out_posts[count])) {
            count++;
        }
    }
    *out_count = count;

    // Extract pagination cursor
    const cJSON *next = cJSON_GetObjectItem(root, "next_cursor");
    if (cJSON_IsString(next) && next->valuestring && next->valuestring[0]) {
        strlcpy(out_next_cursor, next->valuestring, CURSOR_BUF_SIZE);
        *out_has_more = true;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Parsed %zu promoted entries (has_more=%d)", count, *out_has_more);
    return ESP_OK;
}

esp_err_t makapix_promoted_https_refresh(const char *channel_id)
{
    if (!channel_id) return ESP_ERR_INVALID_ARG;

    char display_name[64];
    ps_get_display_name(channel_id, display_name, sizeof(display_name));
    ESP_LOGI(TAG, "Refreshing promoted channel via HTTPS: %s", display_name);

    s_cancel = false;

    // Find channel cache — fail fast before allocating buffers
    channel_cache_t *cache = channel_cache_registry_find(channel_id);
    if (!cache) {
        ESP_LOGW(TAG, "Channel cache not found for '%s'", display_name);
        return ESP_ERR_NOT_FOUND;
    }

    // Resolve paths
    char channels_path[128];
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
    }
    char vault_path[128];
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        strlcpy(vault_path, "/sdcard/p3a/vault", sizeof(vault_path));
    }

    uint32_t cache_size = config_store_get_channel_cache_size();
    if (cache_size == 0) cache_size = 2048;

    // Allocate response buffer (PSRAM preferred)
    char *response_buf = heap_caps_malloc(RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(RESPONSE_BUF_SIZE);
        if (!response_buf) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    // Allocate batch array for parsed posts
    makapix_post_t *posts = heap_caps_malloc(PAGE_LIMIT * sizeof(makapix_post_t),
                                             MALLOC_CAP_SPIRAM);
    if (!posts) {
        posts = malloc(PAGE_LIMIT * sizeof(makapix_post_t));
        if (!posts) {
            ESP_LOGE(TAG, "Failed to allocate posts buffer");
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total_fetched = 0;
    char cursor[CURSOR_BUF_SIZE] = {0};
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        if (s_cancel) {
            ESP_LOGI(TAG, "Refresh cancelled");
            refresh_completed = false;
            break;
        }

        size_t page_count = 0;
        char next_cursor[CURSOR_BUF_SIZE] = {0};
        bool has_more = false;

        esp_err_t err = fetch_promoted_page(response_buf, RESPONSE_BUF_SIZE,
                                            cursor[0] ? cursor : NULL,
                                            posts, &page_count,
                                            next_cursor, &has_more);

        if (s_cancel) {
            ESP_LOGI(TAG, "Refresh cancelled after page fetch");
            refresh_completed = false;
            break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Page fetch failed: %s", esp_err_to_name(err));
            refresh_completed = false;
            break;
        }

        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries returned, done");
            break;
        }

        // Merge batch into channel cache
        esp_err_t merge_err = channel_cache_merge_posts(cache, posts, page_count,
                                                        channels_path, vault_path);
        if (merge_err == ESP_OK) {
            ESP_LOGI(TAG, "Batch merged: %zu entries (cache: %zu entries, %zu available)",
                     page_count,
                     cache->entry_count,
                     cache->available_count);
        } else {
            ESP_LOGW(TAG, "Batch merge failed: %s", esp_err_to_name(merge_err));
        }

        // Signal download manager — downloads can start while we fetch more pages
        download_manager_rescan();

        total_fetched += page_count;

        if (!has_more || next_cursor[0] == '\0') {
            break;
        }

        strlcpy(cursor, next_cursor, sizeof(cursor));

        // Brief delay between pages (matches Giphy pattern)
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(posts);

    // Save metadata only if refresh completed and clock is synchronized
    if (refresh_completed && total_fetched > 0 && sntp_sync_is_synchronized()) {
        channel_metadata_t meta = {
            .last_refresh = time(NULL),
            .cursor = "",
        };
        esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
        if (meta_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
        }
    } else if (refresh_completed && total_fetched > 0) {
        ESP_LOGI(TAG, "Clock not synchronized, deferring metadata save");
    }

    ESP_LOGI(TAG, "Promoted HTTPS refresh %s: %zu fetched, %zu in cache",
             refresh_completed ? "complete" : "incomplete",
             total_fetched, cache->entry_count);

    return (total_fetched > 0) ? ESP_OK : ESP_FAIL;
}
