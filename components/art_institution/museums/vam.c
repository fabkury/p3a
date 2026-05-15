// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/vam.c
 * @brief Victoria and Albert Museum adapter — refresh + IIIF URL build.
 *
 * V&A's API is structurally the simpler of the three museums in this
 * codebase: the search response includes the IIIF identifier
 * (`_primaryImageId`) directly, so refresh stores entries as fully
 * resolved with `extension = 3` — no lazy walk like Rijks needs.
 *
 * Three facet axes (V&A doesn't have a single flat "collection" field):
 *   - collection : V&A's curatorial collections (the closest analogue)
 *   - category   : object-type categories (Prints, Photographs, ...)
 *   - venue      : V&A sites (South Kensington, East, Wedgwood, ...)
 *
 * Pagination is by `page` + `page_size` only (no offset), same as AIC.
 * The IIIF host is `framemark.vam.ac.uk` and serves JPEG only.
 *
 * Reference: https://developers.vam.ac.uk/ and
 * reference/museum-art/ubi-test/js/adapters/vam.js
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "config_store.h"
#include "psram_alloc.h"
#include "sd_path.h"
#include "sntp_sync.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ai_vam";

#define VAM_API_BASE              "https://api.vam.ac.uk/v2"
#define VAM_IIIF_BASE             "https://framemark.vam.ac.uk/collections"
#define VAM_PAGE_LIMIT            100
#define VAM_RESPONSE_BUF_SIZE     (256 * 1024)   // V&A search pages are richer than AIC's
#define VAM_FETCH_MAX_ATTEMPTS    3

static const uint32_t s_fetch_backoff_ms[VAM_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);

// ----- Axis -> filter-param map -------------------------------------------

typedef struct {
    const char *axis;          // adapter axis name
    const char *filter_param;  // V&A search query param
} vam_axis_map_t;

static const vam_axis_map_t VAM_AXES[] = {
    { "collection", "id_collection" },
    { "category",   "id_category"   },
    { "venue",      "id_venue"      },
};

static const char *vam_filter_param_for_axis(const char *axis)
{
    if (!axis) return NULL;
    for (size_t i = 0; i < sizeof(VAM_AXES) / sizeof(VAM_AXES[0]); i++) {
        if (strcmp(VAM_AXES[i].axis, axis) == 0) return VAM_AXES[i].filter_param;
    }
    return NULL;
}

// ----- IIIF URL ------------------------------------------------------------

esp_err_t art_institution_vam_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, VAM_IIIF_BASE "/%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- One-page fetch ------------------------------------------------------

/**
 * @brief Fetch + parse one V&A search page
 *
 * The image_id used by IIIF is the search record's `_primaryImageId`
 * field. Records without it are skipped (the search query already sets
 * images_exist=1, so this should be rare).
 */
static esp_err_t vam_fetch_page(const char *filter_param,
                                const char *term_id,
                                int page,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                bool *has_more)
{
    *out_count = 0;
    *has_more = false;

    // V&A's facet IDs are alphanumeric; ai_url_encode handles them
    // defensively in case of unusual values.
    char encoded_term[128];
    ai_url_encode(term_id, encoded_term, sizeof(encoded_term));

    char url[512];
    int wrote = snprintf(url, sizeof(url),
                         VAM_API_BASE
                         "/objects/search?page=%d&page_size=%d&images_exist=1&%s=%s",
                         page, VAM_PAGE_LIMIT, filter_param, encoded_term);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for filter=%s term=%s", filter_param, term_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (filter=%s term=%.32s)", page, filter_param, term_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < VAM_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying V&A page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, VAM_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "Accept", "application/json");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            continue;
        }

        esp_http_client_fetch_headers(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 429) {
            art_institution_set_rate_limited("vam", 0);
            ESP_LOGW(TAG, "V&A returned 429");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "V&A returned %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "V&A status %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        total_read = ai_drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read < 0) continue;
        if (total_read == 0) continue;
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "V&A response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "V&A truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "V&A page %d fetch failed after %d attempts",
                 page, VAM_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "V&A JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *records = cJSON_GetObjectItem(root, "records");
    if (!cJSON_IsArray(records)) {
        ESP_LOGE(TAG, "V&A response missing 'records' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(records);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *rec = cJSON_GetArrayItem(records, i);
        if (!cJSON_IsObject(rec)) continue;

        const cJSON *image_id = cJSON_GetObjectItem(rec, "_primaryImageId");
        if (!cJSON_IsString(image_id) || !image_id->valuestring[0]) {
            // images_exist=1 in the query should keep this branch rare, but
            // the V&A API isn't perfectly consistent.
            continue;
        }
        const char *iiif_key = image_id->valuestring;
        size_t key_len = strlen(iiif_key);
        if (key_len >= sizeof(out_entries[parsed].iiif_key)) {
            ESP_LOGW(TAG, "V&A _primaryImageId too long (%zu chars), skipping", key_len);
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("vam", iiif_key);
        e->kind = 0;
        e->extension = 3;  // jpg — V&A IIIF serves jpg
        e->created_at = now;
        memcpy(e->iiif_key, iiif_key, key_len + 1);
        parsed++;
    }

    // V&A pagination metadata: info.record_count is the total.
    int total_records = 0;
    const cJSON *info = cJSON_GetObjectItem(root, "info");
    if (cJSON_IsObject(info)) {
        const cJSON *rc = cJSON_GetObjectItem(info, "record_count");
        if (cJSON_IsNumber(rc)) total_records = (int)cJSON_GetNumberValue(rc);
    }
    int seen_so_far = (page - 1) * VAM_PAGE_LIMIT + array_size;
    *has_more = (array_size > 0) && (seen_so_far < total_records);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "V&A page %d: parsed %zu/%d entries (total_records=%d), has_more=%d",
             page, parsed, array_size, total_records, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_vam_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    const char *filter_param = vam_filter_param_for_axis(axis);
    if (!filter_param) {
        ESP_LOGE(TAG, "Unknown V&A axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("vam")) {
        ESP_LOGW(TAG, "V&A rate-limited at refresh start, skipping");
        return ESP_ERR_INVALID_RESPONSE;
    }

    channel_cache_lifecycle_lock();
    bool cache_exists = (channel_cache_registry_find(channel_id) != NULL);
    channel_cache_lifecycle_unlock();
    if (!cache_exists) {
        ESP_LOGW(TAG, "Cache not found for channel '%s'", channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t cache_size = config_store_get_ai_cache_size();
    if (cache_size == 0) cache_size = 1024;
    if (cache_size > 4096) cache_size = 4096;

    char *response_buf = heap_caps_malloc(VAM_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(VAM_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        VAM_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(VAM_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    // V&A supports random-access pagination up to page × page_size ≤ 10 000
    // (see docs/art-institutions/offset-tests/REPORT.md §2.4). Translate the
    // user's record-offset into a starting page. The 10 000-record budget
    // wraps oversized offsets back to the start so the channel never goes
    // empty just because the user's offset is past the cap; we cap the
    // effective offset at 9900 so the smallest page can still fit a full
    // 100-entry response.
    const uint32_t VAM_OFFSET_CAP = 9900;
    uint32_t effective_offset = (channel_offset > VAM_OFFSET_CAP)
        ? (channel_offset % (VAM_OFFSET_CAP + 1))
        : channel_offset;
    int page = (int)(effective_offset / VAM_PAGE_LIMIT) + 1;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        bool has_more = false;
        esp_err_t err = vam_fetch_page(filter_param, term_id, page,
                                       response_buf, VAM_RESPONSE_BUF_SIZE,
                                       page_entries, VAM_PAGE_LIMIT,
                                       &page_count, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries on page %d, done", page);
            break;
        }

        size_t merge_limit = cache_size * 3;
        channel_cache_lifecycle_lock();
        channel_cache_t *page_cache = channel_cache_registry_find(channel_id);
        esp_err_t merge_err = page_cache
            ? art_institution_merge_entries(page_cache, page_entries, page_count, merge_limit)
            : ESP_ERR_NOT_FOUND;
        channel_cache_lifecycle_unlock();

        if (merge_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Cache disappeared mid-refresh for '%s'", channel_id);
            refresh_completed = false;
            break;
        }
        if (merge_err != ESP_OK) {
            ESP_LOGW(TAG, "Merge failed on page %d: %s", page, esp_err_to_name(merge_err));
            refresh_completed = false;
            break;
        }

        for (size_t i = 0; i < page_count && si_count < cache_size; i++) {
            int32_t pid = page_entries[i].post_id;
            ai_si_node_t *existing = NULL;
            HASH_FIND_INT(si_hash, &pid, existing);
            if (!existing) {
                ai_si_node_t *n = psram_malloc(sizeof(ai_si_node_t));
                if (n) {
                    n->post_id = pid;
                    HASH_ADD_INT(si_hash, post_id, n);
                    si_count++;
                }
            }
        }

        total_fetched += page_count;
        ESP_LOGI(TAG, "V&A page %d merged: %zu entries (total %zu)",
                 page, page_count, total_fetched);
        download_manager_rescan();

        page++;
        if (!has_more) break;
        // V&A doesn't publish a rate limit but the API isn't free; be
        // polite between pages, same as AIC.
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "vam");
        }
        channel_cache_lifecycle_unlock();
    }

    {
        ai_si_node_t *node, *tmp;
        HASH_ITER(hh, si_hash, node, tmp) {
            HASH_DEL(si_hash, node);
            free(node);
        }
    }

    if ((refresh_completed || partial_with_content) && sntp_sync_is_synchronized()) {
        char channels_path[128];
        if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
            strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
        }
        channel_metadata_t meta = { .last_refresh = time(NULL), .cursor = "" };
        esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
        if (meta_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
        }
    }

    if (refresh_completed) {
        ESP_LOGI(TAG, "V&A refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "V&A refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "V&A refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
