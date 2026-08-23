// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/cma.c
 * @brief Cleveland Museum of Art adapter — refresh + image URL build.
 *
 * CMA's Open Access API is the first non-IIIF museum in this codebase:
 * there is no IIIF Image API and no size-parameterized delivery. Every
 * search record carries fixed CDN rendition URLs, and the mid-size "web"
 * rendition follows a stable template derivable from the accession number:
 *
 *   https://openaccess-cdn.clevelandart.org/{acc}/{acc}_web.jpg
 *
 * so `build_iiif_url` ignores `longest_side` and emits that template.
 * The web rendition varies in size (~750-1300 px longest side, median
 * ~300 KB, up to ~1 MB); the decoder downscales to the panel.
 *
 * API quirks (verified by live probes 2026-08-23, see
 * reference/museum-art/source/cma/output/report.md):
 *   - `department=` / `type=` filters require the EXACT full name —
 *     partial matches return 0. Two department names exceed the 32-char
 *     playset identifier slot, so those channels store a 32-char
 *     truncation that CMA_TERM_EXPANSION expands back to the full name
 *     (mirrored in webui/museum/cma-terms.json's `query` field).
 *   - `images.web.width`/`height`/`filesize` are STRINGS and occasionally
 *     empty — parsed defensively, 0 = unknown.
 *   - Native `skip`/`limit` pagination; deep offsets work (no AIC-style
 *     cap), so channel_offset uses the Smithsonian page-align+wrap scheme.
 *   - Anonymous, no key, no published rate limit; default 60 s cooldown
 *     on a 429.
 *
 * Reference: https://openaccess-api.clevelandart.org/
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "http_fetch.h"
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

static const char *TAG = "ai_cma";

#define CMA_API_BASE              "https://openaccess-api.clevelandart.org/api/artworks/"
#define CMA_CDN_BASE              "https://openaccess-cdn.clevelandart.org"
#define CMA_PAGE_LIMIT            100
#define CMA_RESPONSE_BUF_SIZE     (256 * 1024)   // ~60-100 KB/page with fields= trimming

extern void download_manager_rescan(void);

// ----- Axis -> filter-param map -------------------------------------------

typedef struct {
    const char *axis;          // adapter axis name
    const char *filter_param;  // CMA search query param
} cma_axis_map_t;

static const cma_axis_map_t CMA_AXES[] = {
    { "department", "department" },
    { "type",       "type"       },
};

static const char *cma_filter_param_for_axis(const char *axis)
{
    if (!axis) return NULL;
    for (size_t i = 0; i < sizeof(CMA_AXES) / sizeof(CMA_AXES[0]); i++) {
        if (strcmp(CMA_AXES[i].axis, axis) == 0) return CMA_AXES[i].filter_param;
    }
    return NULL;
}

// ----- Term-id expansion ---------------------------------------------------

// The playset identifier slot holds 32 chars; these department names are
// longer, and the API demands exact matches. Channels store the 32-char
// truncation as identifier; this table restores the full query value.
// Keep in sync with the `query` fields scripts/build_cma_terms.py emits
// into webui/museum/cma-terms.json (the script prints this table on run).
static const struct {
    const char *term_id;  // 32-char truncation stored in the playset
    const char *full;     // exact name the API requires
} CMA_TERM_EXPANSION[] = {
    { "Egyptian and Ancient Near Easter", "Egyptian and Ancient Near Eastern Art"  },
    { "Modern European Painting and Scu", "Modern European Painting and Sculpture" },
};

static const char *cma_expand_term(const char *term_id)
{
    for (size_t i = 0; i < sizeof(CMA_TERM_EXPANSION) / sizeof(CMA_TERM_EXPANSION[0]); i++) {
        if (strcmp(CMA_TERM_EXPANSION[i].term_id, term_id) == 0) {
            return CMA_TERM_EXPANSION[i].full;
        }
    }
    return term_id;
}

// ----- Image URL -----------------------------------------------------------

esp_err_t art_institution_cma_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    // CMA has no size-parameterized delivery; the web rendition is the
    // only mid-size derivative. longest_side is meaningless here.
    (void)longest_side;

    int n = snprintf(out, len, CMA_CDN_BASE "/%s/%s_web.jpg",
                     entry->iiif_key, entry->iiif_key);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- One-page fetch ------------------------------------------------------

// 429 handler: record the cooldown so the picker and browser both back off.
// Honors a numeric Retry-After when present (the helper parses it), else the
// rate-limit table's default.
static void cma_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    (void)ctx;
    art_institution_set_rate_limited("cma", retry_after_sec);
}

// Parse a CMA numeric-string field ("748", "" or absent) into uint16.
static uint16_t cma_parse_dim(const cJSON *obj, const char *field)
{
    const cJSON *v = cJSON_GetObjectItem(obj, field);
    if (!cJSON_IsString(v) || !v->valuestring[0]) return 0;
    long n = strtol(v->valuestring, NULL, 10);
    if (n <= 0 || n > 0xFFFF) return 0;
    return (uint16_t)n;
}

/**
 * @brief Fetch + parse one CMA search page
 *
 * The iiif_key is the accession number; the web-rendition URL is rebuilt
 * from it at download time. Records missing `accession_number` or
 * `images.web.url` are skipped (has_image=1 should keep this rare, but
 * ~2% of records carry empty image metadata).
 */
static esp_err_t cma_fetch_page(const char *filter_param,
                                const char *term_id,
                                int skip,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                bool *has_more,
                                int *out_total)
{
    *out_count = 0;
    *has_more = false;
    *out_total = 0;

    // Department/type names contain spaces; encode the expanded term.
    char encoded_term[128];
    ai_url_encode(cma_expand_term(term_id), encoded_term, sizeof(encoded_term));

    char url[512];
    int wrote = snprintf(url, sizeof(url),
                         CMA_API_BASE
                         "?cc0=1&has_image=1&skip=%d&limit=%d&%s=%s"
                         "&fields=accession_number,images",
                         skip, CMA_PAGE_LIMIT, filter_param, encoded_term);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for filter=%s term=%s", filter_param, term_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching skip=%d (filter=%s term=%.32s)", skip, filter_param, term_id);

    http_fetch_header_t headers[] = {
        { "Accept", "application/json" },
    };
    http_fetch_request_t fr = {
        .url = url,
        .headers = headers,
        .header_count = 1,
        // Anonymous keyless API consumed in bulk: identify ourselves with a
        // contact address, same string the download path always sends.
        .user_agent = ai_user_agent(),
        .on_rate_limited = cma_on_rate_limited,
    };
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, response_buf_size, &got, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CMA skip=%d fetch failed: %s", skip, esp_err_to_name(err));
        return err;
    }
    int total_read = (int)got;

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "CMA JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "CMA response missing 'data' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(data);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *rec = cJSON_GetArrayItem(data, i);
        if (!cJSON_IsObject(rec)) continue;

        const cJSON *acc = cJSON_GetObjectItem(rec, "accession_number");
        if (!cJSON_IsString(acc) || !acc->valuestring[0]) continue;

        // Require a live web-rendition URL: the download path rebuilds the
        // template from the accession, so a record without images.web.url
        // would 404 at download time.
        const cJSON *images = cJSON_GetObjectItem(rec, "images");
        const cJSON *web = cJSON_IsObject(images)
            ? cJSON_GetObjectItem(images, "web") : NULL;
        const cJSON *web_url = cJSON_IsObject(web)
            ? cJSON_GetObjectItem(web, "url") : NULL;
        if (!cJSON_IsString(web_url) || !web_url->valuestring[0]) continue;

        const char *iiif_key = acc->valuestring;
        size_t key_len = strlen(iiif_key);
        if (key_len >= sizeof(out_entries[parsed].iiif_key)) {
            ESP_LOGW(TAG, "CMA accession too long (%zu chars), skipping", key_len);
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("cma", iiif_key);
        e->kind = 0;
        e->extension = 3;  // jpg — the web rendition is always JPEG
        e->created_at = now;
        e->width = cma_parse_dim(web, "width");    // string-typed, may be empty
        e->height = cma_parse_dim(web, "height");
        memcpy(e->iiif_key, iiif_key, key_len + 1);
        parsed++;
    }

    // CMA pagination metadata: info.total is the filtered result count.
    int total_records = 0;
    const cJSON *info = cJSON_GetObjectItem(root, "info");
    if (cJSON_IsObject(info)) {
        const cJSON *tot = cJSON_GetObjectItem(info, "total");
        if (cJSON_IsNumber(tot)) total_records = (int)cJSON_GetNumberValue(tot);
    }
    *has_more = (array_size > 0) && (skip + array_size < total_records);
    *out_total = total_records;

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "CMA skip=%d: parsed %zu/%d entries (total=%d), has_more=%d",
             skip, parsed, array_size, total_records, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_cma_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    const char *filter_param = cma_filter_param_for_axis(axis);
    if (!filter_param) {
        ESP_LOGE(TAG, "Unknown CMA axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("cma")) {
        ESP_LOGW(TAG, "CMA rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(CMA_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(CMA_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        CMA_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(CMA_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;

    // Align channel_offset to a page boundary so we always fetch full pages.
    // Deep skip works on CMA (verified at skip=40000), so no offset cap —
    // just the Smithsonian-style modulo wrap when the offset overshoots the
    // term's total.
    int starting_skip = (int)((channel_offset / CMA_PAGE_LIMIT) * CMA_PAGE_LIMIT);
    int skip = starting_skip;

    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        bool has_more = false;
        int total_records = 0;
        esp_err_t err = cma_fetch_page(filter_param, term_id, skip,
                                       response_buf, CMA_RESPONSE_BUF_SIZE,
                                       page_entries, CMA_PAGE_LIMIT,
                                       &page_count, &has_more, &total_records);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }

        // On the first page, if the user's channel_offset exceeded the
        // term's total, wrap modulo the total so the channel doesn't go
        // empty. Re-issue the fetch at the wrapped offset.
        if (skip == starting_skip && total_records > 0 &&
            channel_offset >= (uint32_t)total_records && starting_skip != 0) {
            uint32_t effective_offset = channel_offset % (uint32_t)total_records;
            int new_skip = (int)((effective_offset / CMA_PAGE_LIMIT) * CMA_PAGE_LIMIT);
            if (new_skip != starting_skip) {
                ESP_LOGI(TAG, "channel_offset %lu >= total %d; wrapping to skip=%d",
                         (unsigned long)channel_offset, total_records, new_skip);
                skip = new_skip;
                starting_skip = new_skip;
                continue;  // skip merging this probe page
            }
        }

        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries at skip=%d, done", skip);
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
            ESP_LOGW(TAG, "Merge failed at skip=%d: %s", skip, esp_err_to_name(merge_err));
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
        ESP_LOGI(TAG, "CMA skip=%d merged: %zu entries (total %zu)",
                 skip, page_count, total_fetched);
        download_manager_rescan();

        skip += CMA_PAGE_LIMIT;
        if (!has_more) break;
        // No published rate limit, but be polite between pages, same as
        // the other museums.
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "cma");
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
        ESP_LOGI(TAG, "CMA refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "CMA refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "CMA refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
