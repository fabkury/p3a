// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/mia.c
 * @brief Minneapolis Institute of Art adapter — refresh + image URL build.
 *
 * Mia is the second non-IIIF museum (after CMA): search is a raw
 * Elasticsearch passthrough and images are pre-rendered S3 objects at
 * fixed width buckets:
 *
 *   https://1.api.artsmia.org/{400|800|full}/{id}.jpg
 *
 * The device downloads the 800 bucket (~50 KB, 800 px longest side);
 * `build_iiif_url` ignores `longest_side`. The `iiif_key` is Mia's
 * numeric object id (≤7 digits, trivially FAT-safe).
 *
 * Search surface (verified by live probes 2026-08-24, see
 * reference/museum-art/source/mia/output/report.md):
 *   - GET https://search.artsmia.org/{URL-encoded ES query}?size=N&from=M
 *     Standard ES envelope {hits:{total:{value,relation},hits:[{_source}]}}.
 *   - Every query is scoped to `rights_type:"Public Domain" AND
 *     image:valid` (~34.5k works) — the licensing gate is server-side.
 *   - ES window: from+size ≤ 10 000. PAST the window the endpoint fails
 *     inconsistently: sometimes a BARE `[]` (a JSON array, not the
 *     envelope — parsed here as an empty final page), sometimes HTTP
 *     500 (handled by the normal fetch-error path). channel_offset is
 *     capped+wrapped VAM-style so we normally never hit either.
 *   - hits.total.value display-caps at 10 000 with relation "gte".
 *   - Terms (identifiers) are the facet values themselves ("Paintings",
 *     "Asian Art"); a term containing '"' or '\\' would break the
 *     phrase query, so such terms are rejected here and filtered out at
 *     enumeration time by webui/museum/mia.js (dual gate).
 *
 * Browse-side term enumeration is LIVE via the API's aggregations —
 * no baked vocabulary (see mia.js); the firmware accepts any term the
 * browser saved.
 *
 * Reference: https://github.com/artsmia/collection-elasticsearch
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

static const char *TAG = "ai_mia";

#define MIA_API_BASE              "https://search.artsmia.org/"
#define MIA_IMG_BASE              "https://1.api.artsmia.org"
#define MIA_PAGE_LIMIT            50
#define MIA_RESPONSE_BUF_SIZE     (512 * 1024)   // full ES records ~4 KB each, no _source trim
#define MIA_ES_WINDOW             10000          // ES from+size hard window
#define MIA_OFFSET_CAP            9950           // keep a full page inside the window

extern void download_manager_rescan(void);

// ----- Axis allowlist ------------------------------------------------------

// Axis name == ES field name for every supported axis; the list is the
// contract with webui/museum/mia.js.
static const char *MIA_AXES[] = {
    "classification",
    "department",
    "country",
    "style",
};

static bool mia_axis_supported(const char *axis)
{
    if (!axis) return false;
    for (size_t i = 0; i < sizeof(MIA_AXES) / sizeof(MIA_AXES[0]); i++) {
        if (strcmp(MIA_AXES[i], axis) == 0) return true;
    }
    return false;
}

// ----- Image URL -----------------------------------------------------------

esp_err_t art_institution_mia_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    // Only 400/800/full pre-renders exist; the device always takes 800.
    (void)longest_side;

    int n = snprintf(out, len, MIA_IMG_BASE "/800/%s.jpg", entry->iiif_key);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- One-page fetch ------------------------------------------------------

// 429 handler: record the cooldown so the picker and browser both back off.
static void mia_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    (void)ctx;
    art_institution_set_rate_limited("mia", retry_after_sec);
}

/**
 * @brief Fetch + parse one Mia search page
 *
 * The iiif_key is the numeric object id; the 800-bucket URL is rebuilt
 * from it at download time. Records without a numeric `id` or with
 * `image != "valid"` are skipped (the query already filters image:valid,
 * so this should be rare).
 *
 * Outputs total/relation so the caller can wrap oversized offsets and
 * terminate the walk correctly under the 10 000-record display cap.
 */
static esp_err_t mia_fetch_page(const char *axis,
                                const char *term_id,
                                int from,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                bool *has_more,
                                int *out_total,
                                bool *out_total_gte)
{
    *out_count = 0;
    *has_more = false;
    *out_total = 0;
    *out_total_gte = false;

    // Phrase query; term validated by the caller (no '"' or '\').
    char raw_query[160];
    int qn = snprintf(raw_query, sizeof(raw_query),
                      "%s:\"%s\" AND rights_type:\"Public Domain\" AND image:valid",
                      axis, term_id);
    if (qn < 0 || qn >= (int)sizeof(raw_query)) {
        ESP_LOGE(TAG, "Query overflow for axis=%s term=%s", axis, term_id);
        return ESP_FAIL;
    }

    // The whole ES query travels in the URL path segment, fully encoded.
    char encoded_query[384];
    ai_url_encode(raw_query, encoded_query, sizeof(encoded_query));

    char url[512];
    int wrote = snprintf(url, sizeof(url), MIA_API_BASE "%s?size=%d&from=%d",
                         encoded_query, MIA_PAGE_LIMIT, from);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for axis=%s term=%s", axis, term_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching from=%d (axis=%s term=%.32s)", from, axis, term_id);

    http_fetch_header_t headers[] = {
        { "Accept", "application/json" },
    };
    http_fetch_request_t fr = {
        .url = url,
        .headers = headers,
        .header_count = 1,
        // Anonymous keyless API consumed in bulk: identify ourselves with
        // a contact address, same string the download path always sends.
        .user_agent = ai_user_agent(),
        .on_rate_limited = mia_on_rate_limited,
    };
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, response_buf_size, &got, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Mia from=%d fetch failed: %s", from, esp_err_to_name(err));
        return err;
    }
    int total_read = (int)got;

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Mia JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    // Past the ES from+size window the endpoint answers with a bare `[]`
    // instead of the search envelope. Treat any non-object root as a
    // clean empty final page (offset capping should prevent this, but
    // the guard keeps a stray request from reading as an error).
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Mia from=%d returned non-envelope response (past ES window?)", from);
        cJSON_Delete(root);
        return ESP_OK;
    }

    const cJSON *hits_obj = cJSON_GetObjectItem(root, "hits");
    const cJSON *hits = cJSON_IsObject(hits_obj)
        ? cJSON_GetObjectItem(hits_obj, "hits") : NULL;
    if (!cJSON_IsArray(hits)) {
        ESP_LOGE(TAG, "Mia response missing 'hits.hits' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(hits);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *hit = cJSON_GetArrayItem(hits, i);
        const cJSON *src = cJSON_IsObject(hit)
            ? cJSON_GetObjectItem(hit, "_source") : NULL;
        if (!cJSON_IsObject(src)) continue;

        const cJSON *idv = cJSON_GetObjectItem(src, "id");
        if (!cJSON_IsNumber(idv)) continue;
        long idnum = (long)cJSON_GetNumberValue(idv);
        if (idnum <= 0) continue;

        // The query filters image:valid, but keep the belt-and-braces
        // check: a record without a valid image would 404 at download.
        const cJSON *img = cJSON_GetObjectItem(src, "image");
        if (!cJSON_IsString(img) || strcmp(img->valuestring, "valid") != 0) continue;

        char id_str[16];
        snprintf(id_str, sizeof(id_str), "%ld", idnum);

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("mia", id_str);
        e->kind = 0;
        e->extension = 3;  // jpg — the 800 bucket is always JPEG
        e->created_at = now;
        // width/height stay 0 ("unknown"): Mia's metadata carries the
        // ORIGINAL scan dims, not the 800-bucket rendition we download,
        // and no downstream consumer reads institution entry dims.
        memcpy(e->iiif_key, id_str, strlen(id_str) + 1);
        parsed++;
    }

    // ES pagination metadata: hits.total is {value, relation}; value
    // display-caps at 10000 with relation "gte".
    int total_records = 0;
    bool total_gte = false;
    const cJSON *total = cJSON_IsObject(hits_obj)
        ? cJSON_GetObjectItem(hits_obj, "total") : NULL;
    if (cJSON_IsObject(total)) {
        const cJSON *val = cJSON_GetObjectItem(total, "value");
        if (cJSON_IsNumber(val)) total_records = (int)cJSON_GetNumberValue(val);
        const cJSON *rel = cJSON_GetObjectItem(total, "relation");
        if (cJSON_IsString(rel) && strcmp(rel->valuestring, "gte") == 0) total_gte = true;
    } else if (cJSON_IsNumber(total)) {
        // Defensive: some ES versions return a plain number.
        total_records = (int)cJSON_GetNumberValue(total);
    }

    // Terminate on: empty page, the hard ES window, or the exact total.
    // When relation is "gte" the true total is beyond the window, so the
    // window is the only bound.
    *has_more = (array_size > 0) &&
                (from + array_size < MIA_ES_WINDOW) &&
                (total_gte || (from + array_size < total_records));
    *out_total = total_records;
    *out_total_gte = total_gte;

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "Mia from=%d: parsed %zu/%d entries (total=%d%s), has_more=%d",
             from, parsed, array_size, total_records, total_gte ? "+" : "",
             (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_mia_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    if (!mia_axis_supported(axis)) {
        ESP_LOGE(TAG, "Unknown Mia axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    // A '"' or '\' inside the term would break the phrase query. The
    // browse adapter drops such terms at enumeration; reject here too in
    // case a hand-crafted playset carries one.
    if (strpbrk(term_id, "\"\\") != NULL) {
        ESP_LOGE(TAG, "Mia term contains unsupported characters: %.32s", term_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited("mia")) {
        ESP_LOGW(TAG, "Mia rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(MIA_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(MIA_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        MIA_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(MIA_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;

    // Two-stage offset handling:
    //   1. Cap-and-wrap into the ES from+size window (VAM pattern) so a
    //      deep offset never lands past 10 000 where the API answers `[]`.
    //   2. On the first page, if the term's EXACT total (relation "eq")
    //      is still smaller than the capped offset, wrap modulo the total
    //      (CMA pattern). When relation is "gte" the true total exceeds
    //      the window, so the capped offset is always in range.
    uint32_t effective_offset = (channel_offset > MIA_OFFSET_CAP)
        ? (channel_offset % (MIA_OFFSET_CAP + 1))
        : channel_offset;
    int starting_from = (int)((effective_offset / MIA_PAGE_LIMIT) * MIA_PAGE_LIMIT);
    int from = starting_from;

    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        bool has_more = false;
        int total_records = 0;
        bool total_gte = false;
        esp_err_t err = mia_fetch_page(axis, term_id, from,
                                       response_buf, MIA_RESPONSE_BUF_SIZE,
                                       page_entries, MIA_PAGE_LIMIT,
                                       &page_count, &has_more,
                                       &total_records, &total_gte);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }

        if (from == starting_from && !total_gte && total_records > 0 &&
            effective_offset >= (uint32_t)total_records && starting_from != 0) {
            uint32_t wrapped = effective_offset % (uint32_t)total_records;
            int new_from = (int)((wrapped / MIA_PAGE_LIMIT) * MIA_PAGE_LIMIT);
            if (new_from != starting_from) {
                ESP_LOGI(TAG, "channel_offset %lu >= total %d; wrapping to from=%d",
                         (unsigned long)channel_offset, total_records, new_from);
                from = new_from;
                starting_from = new_from;
                continue;  // skip merging this probe page
            }
        }

        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries at from=%d, done", from);
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
            ESP_LOGW(TAG, "Merge failed at from=%d: %s", from, esp_err_to_name(merge_err));
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
        ESP_LOGI(TAG, "Mia from=%d merged: %zu entries (total %zu)",
                 from, page_count, total_fetched);
        download_manager_rescan();

        from += MIA_PAGE_LIMIT;
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
            art_institution_evict_orphans(evict_cache, si_hash, "mia");
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
        ESP_LOGI(TAG, "Mia refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "Mia refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Mia refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
