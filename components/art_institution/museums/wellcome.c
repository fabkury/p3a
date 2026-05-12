// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/wellcome.c
 * @brief Wellcome Collection adapter — refresh + IIIF URL build.
 *
 * Wellcome's catalogue API is structurally simple: each result includes
 * `items[].locations[]` where `locationType.id == "iiif-image"` carries
 * the IIIF service URL. Refresh stores entries fully resolved with
 * `extension = 3` — no lazy walk like Rijks needs.
 *
 * Four facet axes (workType, genres, subjects, contributors). For
 * workType, the filter value is the term `id` (e.g. `k`); for the others,
 * the filter value is the term `label` itself (Wellcome doesn't expose a
 * stable short id for those axes). The browse modal hides labels
 * exceeding 32 chars so the playset identifier[33] slot fits.
 *
 * Reference: https://developers.wellcomecollection.org/api and
 * reference/museum-art/source/wellcome/run.py
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

static const char *TAG = "ai_wellcome";

#define WELLCOME_API_BASE           "https://api.wellcomecollection.org/catalogue/v2"
#define WELLCOME_IIIF_BASE          "https://iiif.wellcomecollection.org/image/"
#define WELLCOME_PAGE_LIMIT         100
#define WELLCOME_RESPONSE_BUF_SIZE  (256 * 1024)
#define WELLCOME_FETCH_MAX_ATTEMPTS 3

static const uint32_t s_fetch_backoff_ms[WELLCOME_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);

// ----- Axis -> filter-param map -------------------------------------------

typedef struct {
    const char *axis;          // adapter axis name
    const char *filter_param;  // Wellcome query parameter
} wellcome_axis_map_t;

static const wellcome_axis_map_t WELLCOME_AXES[] = {
    { "workType",     "workType"                  },
    { "genres",       "genres.label"              },
    { "subjects",     "subjects.label"            },
    { "contributors", "contributors.agent.label"  },
};

static const char *wellcome_filter_param_for_axis(const char *axis)
{
    if (!axis) return NULL;
    for (size_t i = 0; i < sizeof(WELLCOME_AXES) / sizeof(WELLCOME_AXES[0]); i++) {
        if (strcmp(WELLCOME_AXES[i].axis, axis) == 0) return WELLCOME_AXES[i].filter_param;
    }
    return NULL;
}

// ----- IIIF URL -----------------------------------------------------------

esp_err_t art_institution_wellcome_build_iiif_url(const institution_channel_entry_t *entry,
                                                  int longest_side,
                                                  char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, WELLCOME_IIIF_BASE "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Helpers ------------------------------------------------------------

/**
 * Walk a Wellcome result's items[].locations[] looking for an
 * iiif-image location, and capture the vid from the URL.
 *
 * Expected URL shape: https://iiif.wellcomecollection.org/image/{vid}[/info.json]
 *
 * Returns true and writes the vid into out on success.
 */
static bool extract_wellcome_vid(const cJSON *work, char *out, size_t out_len)
{
    if (!work || !out || out_len == 0) return false;
    const cJSON *items = cJSON_GetObjectItem(work, "items");
    if (!cJSON_IsArray(items)) return false;
    int items_n = cJSON_GetArraySize(items);
    static const char prefix[] = "https://iiif.wellcomecollection.org/image/";
    const size_t prefix_len = sizeof(prefix) - 1;

    for (int i = 0; i < items_n; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;
        const cJSON *locations = cJSON_GetObjectItem(item, "locations");
        if (!cJSON_IsArray(locations)) continue;
        int locs_n = cJSON_GetArraySize(locations);
        for (int j = 0; j < locs_n; j++) {
            const cJSON *loc = cJSON_GetArrayItem(locations, j);
            if (!cJSON_IsObject(loc)) continue;
            const cJSON *ltype = cJSON_GetObjectItem(loc, "locationType");
            if (!cJSON_IsObject(ltype)) continue;
            const cJSON *ltype_id = cJSON_GetObjectItem(ltype, "id");
            if (!cJSON_IsString(ltype_id) || strcmp(ltype_id->valuestring, "iiif-image") != 0) continue;
            const cJSON *url = cJSON_GetObjectItem(loc, "url");
            if (!cJSON_IsString(url) || !url->valuestring[0]) continue;

            const char *u = url->valuestring;
            if (strncmp(u, prefix, prefix_len) != 0) continue;
            const char *rest = u + prefix_len;
            const char *slash = strchr(rest, '/');
            size_t vid_len = slash ? (size_t)(slash - rest) : strlen(rest);
            if (vid_len == 0 || vid_len >= out_len) continue;
            memcpy(out, rest, vid_len);
            out[vid_len] = '\0';
            return true;
        }
    }
    return false;
}

// ----- One-page fetch -----------------------------------------------------

/**
 * Fetch + parse one Wellcome /works page.
 *
 * The filter value goes straight onto the URL after percent-encoding —
 * for `workType` it's a short id (`k`/`q`/...), for the other axes it's
 * the term label itself which may contain spaces or punctuation. The
 * 32-char ceiling is enforced browser-side so we don't need to truncate
 * here; we'd see at most 32 chars before encoding.
 */
static esp_err_t wellcome_fetch_page(const char *filter_param,
                                     const char *filter_value,
                                     int page,
                                     char *response_buf,
                                     size_t response_buf_size,
                                     institution_channel_entry_t *out_entries,
                                     size_t max_entries,
                                     size_t *out_count,
                                     int *out_total_results,
                                     bool *has_more)
{
    *out_count = 0;
    *has_more = false;
    if (out_total_results) *out_total_results = 0;

    char encoded_value[256];
    ai_url_encode(filter_value, encoded_value, sizeof(encoded_value));

    char url[640];
    int wrote = snprintf(url, sizeof(url),
                         WELLCOME_API_BASE
                         "/works?page=%d&pageSize=%d"
                         "&items.locations.locationType=iiif-image"
                         "&include=items"
                         "&%s=%s",
                         page, WELLCOME_PAGE_LIMIT,
                         filter_param, encoded_value);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for filter=%s", filter_param);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (filter=%s value=%.32s)", page, filter_param, filter_value);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < WELLCOME_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying Wellcome page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, WELLCOME_FETCH_MAX_ATTEMPTS);
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
            char *retry_after = NULL;
            uint32_t cooldown = 0;
            if (esp_http_client_get_header(client, "Retry-After", &retry_after) == ESP_OK) {
                cooldown = ai_parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("wellcome", cooldown);
            ESP_LOGW(TAG, "Wellcome returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "Wellcome returned %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "Wellcome status %d on page %d", status, page);
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
            ESP_LOGE(TAG, "Wellcome response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "Wellcome truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "Wellcome page %d fetch failed after %d attempts",
                 page, WELLCOME_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Wellcome JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        ESP_LOGE(TAG, "Wellcome response missing 'results' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int total_results = 0;
    const cJSON *total_node = cJSON_GetObjectItem(root, "totalResults");
    if (cJSON_IsNumber(total_node)) total_results = (int)cJSON_GetNumberValue(total_node);
    if (out_total_results) *out_total_results = total_results;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(results);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *work = cJSON_GetArrayItem(results, i);
        if (!cJSON_IsObject(work)) continue;

        char vid[sizeof(out_entries[0].iiif_key)] = {0};
        if (!extract_wellcome_vid(work, vid, sizeof(vid))) {
            // items.locations.locationType=iiif-image filter should keep this
            // branch rare, but Wellcome isn't perfectly consistent.
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("wellcome", vid);
        e->kind = 0;
        e->extension = 3;
        e->created_at = now;
        strlcpy(e->iiif_key, vid, sizeof(e->iiif_key));
        parsed++;
    }

    int seen_so_far = (page - 1) * WELLCOME_PAGE_LIMIT + array_size;
    *has_more = (array_size > 0) && (seen_so_far < total_results);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "Wellcome page %d: parsed %zu/%d entries (totalResults=%d), has_more=%d",
             page, parsed, array_size, total_results, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_wellcome_refresh_channel(const char *channel_id,
                                                   const char *axis,
                                                   const char *term_id)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    const char *filter_param = wellcome_filter_param_for_axis(axis);
    if (!filter_param) {
        ESP_LOGE(TAG, "Unknown Wellcome axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("wellcome")) {
        ESP_LOGW(TAG, "Wellcome rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(WELLCOME_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(WELLCOME_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        WELLCOME_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(WELLCOME_PAGE_LIMIT * sizeof(institution_channel_entry_t));
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

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        int total_results = 0;
        bool has_more = false;
        esp_err_t err = wellcome_fetch_page(filter_param, term_id, page,
                                            response_buf, WELLCOME_RESPONSE_BUF_SIZE,
                                            page_entries, WELLCOME_PAGE_LIMIT,
                                            &page_count, &total_results, &has_more);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page_count == 0 && !has_more) {
            ESP_LOGI(TAG, "No entries on page %d, done", page);
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
            ESP_LOGI(TAG, "Wellcome page %d merged: %zu entries (total %zu)",
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
            art_institution_evict_orphans(evict_cache, si_hash, "wellcome");
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
        ESP_LOGI(TAG, "Wellcome refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "Wellcome refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Wellcome refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
