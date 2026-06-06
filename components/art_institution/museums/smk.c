// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/smk.c
 * @brief SMK (Statens Museum for Kunst) adapter — refresh + IIIF URL build.
 *
 * SMK's search API returns results with `image_iiif_id` as a full URL
 * (e.g. `https://iip.smk.dk/iiif/jp2/bc386p50w_kksgb22235.tif.jp2`).
 * We store only the filename — everything after the last `/jp2/` — as
 * the iiif_key, and prepend the standard prefix at URL-build time.
 *
 * Single axis (`collection`). Filter syntax is
 * `filters=[collection:NAME],[has_image:true]`; the whole expression is
 * one query-value, so we build it unencoded and percent-encode the
 * entire string once.
 *
 * Note on JPEG vs WebP: SMK's IIPImage backend advertises WebP in
 * info.json but returns HTTP 400 for `.webp` requests (captured in
 * reference/museum-art/source/smk/output/report.md). Stay on JPEG.
 *
 * Reference: https://api.smk.dk/api/v1/docs/ and
 * reference/museum-art/source/smk/run.py
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "download_manager.h"
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

#include "http_fetch.h"

static const char *TAG = "ai_smk";

#define SMK_API_BASE           "https://api.smk.dk/api/v1"
#define SMK_IIIF_PREFIX        "https://iip.smk.dk/iiif/jp2/"
// SMK returns full records (~4 KB each: production, materials, notes, titles,
// ...) and its `fields` query param won't trim them down to image_iiif_id the
// way AIC/HAM shrink their responses (it returns empty items), so the page
// can't be reduced request-side. A 50-record page measures ~205 KB for
// metadata-rich collections (e.g. "Stroes fortegnelse"), so keep rows modest
// (cf. Smithsonian rows=50) and size the buffer to hold it with headroom.
// Note: image_iiif_id is present on most — not all — records; the parser
// skips those without it (some carry only a UUID thumbnail).
#define SMK_PAGE_LIMIT         50
#define SMK_RESPONSE_BUF_SIZE  (512 * 1024)

extern void download_manager_rescan(void);

// ----- IIIF URL -----------------------------------------------------------

esp_err_t art_institution_smk_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, SMK_IIIF_PREFIX "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Helpers ------------------------------------------------------------

/**
 * Extract the IIIF filename from an SMK image_iiif_id URL.
 *
 * Verifies the URL contains `/iiif/jp2/`; stores the substring after the
 * last `/jp2/` as the iiif_key (e.g. `bc386p50w_kksgb22235.tif.jp2`).
 * Returns false if the URL doesn't match the expected shape or the
 * extracted filename overflows out_len.
 */
static bool extract_smk_filename(const char *image_iiif_id, char *out, size_t out_len)
{
    if (!image_iiif_id || !out || out_len == 0) return false;
    static const char marker[] = "/iiif/jp2/";
    const size_t marker_len = sizeof(marker) - 1;

    // Find the LAST occurrence of /iiif/jp2/ — defensive against URLs
    // that contain the marker as a path segment more than once.
    const char *cursor = image_iiif_id;
    const char *last = NULL;
    while ((cursor = strstr(cursor, marker)) != NULL) {
        last = cursor;
        cursor += marker_len;
    }
    if (!last) return false;

    const char *filename = last + marker_len;
    size_t filename_len = strlen(filename);
    if (filename_len == 0 || filename_len >= out_len) return false;

    memcpy(out, filename, filename_len + 1);
    return true;
}

// ----- One-page fetch -----------------------------------------------------

static void smk_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "SMK rate-limited (Retry-After %us)", (unsigned)retry_after_sec);
    art_institution_set_rate_limited("smk", retry_after_sec);
}

static esp_err_t smk_fetch_page(const char *collection_name,
                                int offset,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                int *out_found,
                                bool *has_more)
{
    *out_count = 0;
    *has_more = false;
    if (out_found) *out_found = 0;

    // Build the unencoded filter expression, then percent-encode the
    // whole thing once. SMK's filters syntax embeds brackets/colons/commas
    // that must be encoded; ai_url_encode encodes everything outside the
    // RFC 3986 unreserved set, which is exactly what we want here.
    char filter_expr[384];
    int fwrote = snprintf(filter_expr, sizeof(filter_expr),
                          "[collection:%s],[has_image:true]", collection_name);
    if (fwrote < 0 || fwrote >= (int)sizeof(filter_expr)) {
        ESP_LOGE(TAG, "Filter expression overflow for collection='%s'", collection_name);
        return ESP_FAIL;
    }

    char encoded_filter[1024];  // 3x expansion worst case
    ai_url_encode(filter_expr, encoded_filter, sizeof(encoded_filter));

    char url[1280];
    int wrote = snprintf(url, sizeof(url),
                         SMK_API_BASE
                         "/art/search?keys=*&offset=%d&rows=%d&filters=%s",
                         offset, SMK_PAGE_LIMIT, encoded_filter);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for collection='%s'", collection_name);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching offset %d (collection=%.32s)", offset, collection_name);

    http_fetch_header_t headers[] = {
        { "Accept", "application/json" },
    };
    http_fetch_request_t fr = {
        .url = url,
        .headers = headers,
        .header_count = 1,
        .on_rate_limited = smk_on_rate_limited,
    };
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, response_buf_size, &got, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SMK offset %d fetch failed: %s", offset, esp_err_to_name(err));
        return err;
    }
    int total_read = (int)got;

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "SMK JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        ESP_LOGE(TAG, "SMK response missing 'items' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int found = 0;
    const cJSON *found_node = cJSON_GetObjectItem(root, "found");
    if (cJSON_IsNumber(found_node)) found = (int)cJSON_GetNumberValue(found_node);
    if (out_found) *out_found = found;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(items);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;

        const cJSON *iiif_id = cJSON_GetObjectItem(item, "image_iiif_id");
        if (!cJSON_IsString(iiif_id) || !iiif_id->valuestring[0]) continue;

        char filename[sizeof(out_entries[0].iiif_key)] = {0};
        if (!extract_smk_filename(iiif_id->valuestring, filename, sizeof(filename))) {
            // Either the URL doesn't contain /iiif/jp2/ or the filename
            // exceeds the 48-byte iiif_key slot; skip defensively.
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("smk", filename);
        e->kind = 0;
        e->extension = 3;
        e->created_at = now;
        strlcpy(e->iiif_key, filename, sizeof(e->iiif_key));
        parsed++;
    }

    int seen_so_far = offset + array_size;
    *has_more = (array_size > 0) && (seen_so_far < found);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "SMK offset %d: parsed %zu/%d entries (found=%d), has_more=%d",
             offset, parsed, array_size, found, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_smk_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset)
{
    (void)axis;  // SMK has one axis (collection); axis is informational here.
    // SMK adapter has not been updated to honor channel_offset yet; accept
    // the parameter so the dispatch table signature matches but ignore it.
    (void)channel_offset;
    if (!channel_id || !term_id || term_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (art_institution_is_rate_limited("smk")) {
        ESP_LOGW(TAG, "SMK rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(SMK_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(SMK_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        SMK_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(SMK_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    int offset = 0;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        int found = 0;
        bool has_more = false;
        esp_err_t err = smk_fetch_page(term_id, offset,
                                       response_buf, SMK_RESPONSE_BUF_SIZE,
                                       page_entries, SMK_PAGE_LIMIT,
                                       &page_count, &found, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0 && !has_more) {
            ESP_LOGI(TAG, "No entries at offset %d, done", offset);
            break;
        }

        if (page_count > 0) {
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
                ESP_LOGW(TAG, "Merge failed at offset %d: %s", offset, esp_err_to_name(merge_err));
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
            ESP_LOGI(TAG, "SMK offset %d merged: %zu entries (total %zu)",
                     offset, page_count, total_fetched);
            download_manager_rescan();
        }

        offset += SMK_PAGE_LIMIT;
        if (!has_more) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "smk");
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
        esp_err_t path_err = sd_path_get_channel(channels_path, sizeof(channels_path));
        if (path_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot resolve channel directory (%s) - skipping metadata save",
                     esp_err_to_name(path_err));
        } else {
            channel_metadata_t meta = { .last_refresh = time(NULL), .cursor = "" };
            esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
            if (meta_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
            }
        }
    }

    if (refresh_completed) {
        ESP_LOGI(TAG, "SMK refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "SMK refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "SMK refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
