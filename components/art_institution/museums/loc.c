// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/loc.c
 * @brief Library of Congress adapter — refresh + IIIF URL build.
 *
 * Single axis (`format`). LoC's `/search/` API surfaces a IIIF URL inline
 * on a fraction of results (~8% for photo/print/drawing, ~28% for
 * manuscript/mixed material). Results without an `image-services/iiif`
 * URL are silently dropped at parse time; results whose IIIF id is 48
 * chars or longer are also dropped (see docs/deferred/loc-iiif-key-48-char.md).
 *
 * Reference: docs/art-institutions/loc-channel-design.md and
 * docs/art-institutions/loc-investigation/REPORT.md.
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
#include "version.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ai_loc";

#define LOC_IIIF_BASE "https://tile.loc.gov/image-services/iiif"

#define LOC_API_BASE              "https://www.loc.gov"
#define LOC_PAGE_LIMIT            100
#define LOC_RESPONSE_BUF_SIZE     (256 * 1024)
#define LOC_FETCH_MAX_ATTEMPTS    3
#define LOC_MAX_PAGES             500  // defensive cap; see loc-channel-design.md §3
#define LOC_IIIF_URL_PREFIX       "https://tile.loc.gov/image-services/iiif/"

static const uint32_t s_fetch_backoff_ms[LOC_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);

static const char *loc_user_agent(void)
{
    static char s_ua[64];
    static bool s_inited = false;
    if (!s_inited) {
        snprintf(s_ua, sizeof(s_ua), "p3a/%s (pub@kury.dev)", FW_VERSION_STRING);
        s_inited = true;
    }
    return s_ua;
}

/**
 * Extract the IIIF id from one LoC image URL.
 *
 * Returns true if @p image_url begins with `https://tile.loc.gov/image-services/iiif/`,
 * the path segment up to the next `/` is non-empty, and that segment
 * fits in (out_len - 1). Writes the segment + NUL into @p out on
 * success.
 *
 * The 48-char `iiif_key` cap is enforced by `out_len < 48` — see
 * docs/deferred/loc-iiif-key-48-char.md for context.
 */
static bool extract_loc_iiif_id(const char *image_url, char *out, size_t out_len)
{
    if (!image_url || !out || out_len == 0) return false;
    static const char prefix[] = LOC_IIIF_URL_PREFIX;
    const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(image_url, prefix, prefix_len) != 0) return false;

    const char *rest = image_url + prefix_len;
    const char *slash = strchr(rest, '/');
    size_t id_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (id_len == 0 || id_len >= out_len) return false;

    memcpy(out, rest, id_len);
    out[id_len] = '\0';
    return true;
}

/**
 * Walk one search-result's image_url[] and resources[].image fields
 * looking for an LoC IIIF URL. Writes the extracted id into @p out
 * and returns true on the first match; false if no IIIF URL is
 * present or every candidate exceeds the iiif_key slot.
 */
static bool pick_loc_iiif_id(const cJSON *result, char *out, size_t out_len)
{
    if (!result) return false;

    const cJSON *image_url = cJSON_GetObjectItem(result, "image_url");
    if (cJSON_IsArray(image_url)) {
        int n = cJSON_GetArraySize(image_url);
        for (int i = 0; i < n; i++) {
            const cJSON *u = cJSON_GetArrayItem(image_url, i);
            if (cJSON_IsString(u) && u->valuestring &&
                extract_loc_iiif_id(u->valuestring, out, out_len)) {
                return true;
            }
        }
    } else if (cJSON_IsString(image_url) && image_url->valuestring) {
        if (extract_loc_iiif_id(image_url->valuestring, out, out_len)) {
            return true;
        }
    }

    const cJSON *resources = cJSON_GetObjectItem(result, "resources");
    if (cJSON_IsArray(resources)) {
        int n = cJSON_GetArraySize(resources);
        for (int i = 0; i < n; i++) {
            const cJSON *r = cJSON_GetArrayItem(resources, i);
            if (!cJSON_IsObject(r)) continue;
            const cJSON *img = cJSON_GetObjectItem(r, "image");
            if (cJSON_IsString(img) && img->valuestring &&
                extract_loc_iiif_id(img->valuestring, out, out_len)) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Fetch and parse one LoC `/search/` page.
 *
 * URL shape:
 *   GET /search/?fo=json&c=100&sp=N&fa=original-format:<lowercase title>
 *
 * Returns ESP_OK with *out_count set on success (possibly 0 — pages
 * commonly yield few IIIF-bearing entries; the refresh loop continues
 * paging until cache_size entries are collected or end-of-results).
 * Returns ESP_ERR_INVALID_RESPONSE on 429 (cooldown engaged before
 * return), ESP_ERR_NOT_ALLOWED on 401/403, ESP_FAIL on parse error or
 * exhausted retries.
 */
static esp_err_t loc_fetch_page(const char *term_id,
                                int page,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                int *out_total,
                                bool *has_more)
{
    *out_count = 0;
    *has_more = false;
    if (out_total) *out_total = 0;

    // term_id is the LoC facet value verbatim (e.g. "photo, print, drawing").
    // ai_url_encode percent-encodes everything outside the unreserved set,
    // which is what LoC's API accepts; see loc-investigation REPORT §Q1.
    char encoded_term[160];
    ai_url_encode(term_id, encoded_term, sizeof(encoded_term));

    char url[640];
    int wrote = snprintf(url, sizeof(url),
                         LOC_API_BASE
                         "/search/?fo=json&c=%d&sp=%d&fa=original-format%%3A%s",
                         LOC_PAGE_LIMIT, page, encoded_term);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for term=%s page=%d", term_id, page);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (term=%.40s)", page, term_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < LOC_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying LoC page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, LOC_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "User-Agent", loc_user_agent());
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
            char *retry_after = NULL;
            uint32_t cooldown = 0;
            if (esp_http_client_get_header(client, "Retry-After", &retry_after) == ESP_OK) {
                cooldown = ai_parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("loc", cooldown);
            ESP_LOGW(TAG, "LoC returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "LoC returned %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "LoC status %d on page %d", status, page);
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
            ESP_LOGE(TAG, "LoC response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "LoC truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "LoC page %d fetch failed after %d attempts",
                 page, LOC_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "LoC JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        ESP_LOGE(TAG, "LoC response missing 'results' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int total_records = 0;
    const cJSON *pag = cJSON_GetObjectItem(root, "pagination");
    if (cJSON_IsObject(pag)) {
        const cJSON *t = cJSON_GetObjectItem(pag, "total");
        if (cJSON_IsNumber(t)) total_records = (int)cJSON_GetNumberValue(t);
    }
    if (out_total) *out_total = total_records;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(results);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *r = cJSON_GetArrayItem(results, i);
        if (!cJSON_IsObject(r)) continue;

        char iiif_key[sizeof(out_entries[0].iiif_key)];
        if (!pick_loc_iiif_id(r, iiif_key, sizeof(iiif_key))) {
            // No IIIF URL, or extracted id >= 48 chars; silently skip
            // per the design (see loc-channel-design.md §5.2, §5.4).
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("loc", iiif_key);
        e->kind = 0;
        e->extension = 3;  // jpg — LoC's IIIF reliably serves JPEG
        e->created_at = now;
        strlcpy(e->iiif_key, iiif_key, sizeof(e->iiif_key));
        parsed++;
    }

    int seen_so_far = (page - 1) * LOC_PAGE_LIMIT + array_size;
    *has_more = (array_size > 0) && (seen_so_far < total_records);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "LoC page %d: parsed %zu/%d entries (total=%d), has_more=%d",
             page, parsed, array_size, total_records, (int)*has_more);
    return ESP_OK;
}

esp_err_t art_institution_loc_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, LOC_IIIF_BASE "/%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t art_institution_loc_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    // LoC v1 exposes a single axis. Reject anything else so a malformed
    // playset (or a hypothetical future axis a user hand-edited in) gets
    // a clear error rather than silently fetching the wrong thing.
    if (strcmp(axis, "format") != 0) {
        ESP_LOGE(TAG, "Unknown LoC axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("loc")) {
        ESP_LOGW(TAG, "LoC rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(LOC_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(LOC_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        LOC_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(LOC_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    int page = 1;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size && page <= LOC_MAX_PAGES) {
        size_t page_count = 0;
        int total_records = 0;
        bool has_more = false;
        esp_err_t err = loc_fetch_page(term_id, page,
                                       response_buf, LOC_RESPONSE_BUF_SIZE,
                                       page_entries, LOC_PAGE_LIMIT,
                                       &page_count, &total_records, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0 && !has_more) {
            ESP_LOGI(TAG, "No more entries at page %d, done", page);
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
            ESP_LOGI(TAG, "LoC page %d merged: %zu entries (total %zu)",
                     page, page_count, total_fetched);
            download_manager_rescan();
        }

        page++;
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
            art_institution_evict_orphans(evict_cache, si_hash, "loc");
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
        ESP_LOGI(TAG, "LoC refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "LoC refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "LoC refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
