// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/ham.c
 * @brief Harvard Art Museums (HAM) adapter — refresh + IIIF URL build.
 *
 * Implements the per-museum spec in
 * docs/art-institutions/ham-investigation/REPORT.md
 * §"Per-museum specification draft for finalized-design.md".
 *
 * HAM's API has the uniform shape that lets the browser-side adapter do
 * hybrid axis discovery — every facet endpoint (`/classification`,
 * `/century`, etc.) doubles as the filter parameter name on `/object`.
 * The device side just needs to translate (channel_id, axis, term_id)
 * into `/object?<axis>=<term_id>` and paginate.
 *
 * BYOK: HAM ships no API key. The user enters their personal key in
 * settings.html and it's stored in NVS under `ham_api_key`. When the
 * key is empty, refresh is a no-op (ESP_LOGI + return ESP_OK so the
 * dispatcher waits the full ai_refresh_sec window before retrying).
 *
 * Image flow: listing returns `primaryimageurl` of the form
 * `https://nrs.harvard.edu/urn-3:HUAM:NNNN_dynmc`. We store only the
 * URN suffix (e.g. `urn-3:HUAM:79762_dynmc`, 17-26 chars) in iiif_key.
 * `build_iiif_url` prepends the host and appends the IIIF size syntax.
 * At download time the NRS URL 303-redirects to
 * `ids.lib.harvard.edu`; the download path's redirect shim handles it.
 *
 * Permission gate: `q=imagepermissionlevel:0` is required on every
 * listing call to suppress records whose image is permission-restricted
 * (no `primaryimageurl`, no `images[]`). Without this filter, roughly
 * half of `hasimage=1` records come back with no displayable URL.
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

static const char *TAG = "ai_ham";

#define HAM_API_BASE              "https://api.harvardartmuseums.org"
#define HAM_NRS_PREFIX            "https://nrs.harvard.edu/"
#define HAM_PAGE_LIMIT            100
#define HAM_RESPONSE_BUF_SIZE     (256 * 1024)
#define HAM_API_KEY_MAX           64

extern void download_manager_rescan(void);

// ----- IIIF URL ------------------------------------------------------------

esp_err_t art_institution_ham_build_iiif_url(const institution_channel_entry_t *entry,
                                             int longest_side,
                                             char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    // iiif_key is the URN portion (e.g. `urn-3:HUAM:79762_dynmc`).
    // The NRS host preserves IIIF Image API v2 path syntax when appended,
    // 303-redirecting to ids.lib.harvard.edu — handled by the download shim.
    int n = snprintf(out, len, HAM_NRS_PREFIX "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Extract URN from a HAM primaryimageurl ------------------------------

/**
 * @brief Return the URN portion of an NRS image URL, or NULL on mismatch
 *
 * Input: `https://nrs.harvard.edu/urn-3:HUAM:79762_dynmc`
 * Output: `urn-3:HUAM:79762_dynmc` (a pointer into the input string)
 *
 * Returns NULL if the URL doesn't start with the NRS prefix.
 */
static const char *ham_extract_urn(const char *full_url)
{
    if (!full_url) return NULL;
    static const size_t prefix_len = sizeof(HAM_NRS_PREFIX) - 1;
    if (strncmp(full_url, HAM_NRS_PREFIX, prefix_len) != 0) return NULL;
    return full_url + prefix_len;
}

// ----- One-page fetch ------------------------------------------------------

static void ham_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "HAM rate-limited (Retry-After %us)", (unsigned)retry_after_sec);
    art_institution_set_rate_limited("ham", retry_after_sec);
}

/**
 * @brief Fetch + parse one HAM /object search page
 *
 * Builds the URL with all the mandatory query params (apikey, hasimage,
 * q=imagepermissionlevel:0, sort=id, the per-axis filter, and a tight
 * `fields` selection to keep response size down).
 */
static esp_err_t ham_fetch_page(const char *axis,
                                const char *term_id,
                                const char *api_key,
                                int page,
                                char *response_buf,
                                size_t response_buf_size,
                                institution_channel_entry_t *out_entries,
                                size_t max_entries,
                                size_t *out_count,
                                bool *has_more,
                                int *out_total_records)
{
    *out_count = 0;
    *has_more = false;
    if (out_total_records) *out_total_records = 0;

    // Encode the term_id defensively (HAM ids are typically numeric but
    // some axes like worktype/group use string ids).
    char encoded_term[128];
    ai_url_encode(term_id, encoded_term, sizeof(encoded_term));

    // `imagepermissionlevel:0` needs the colon percent-encoded.
    char encoded_q[64];
    ai_url_encode("imagepermissionlevel:0", encoded_q, sizeof(encoded_q));

    // Note: apikey here is raw; HAM accepts the UUID format unencoded.
    char url[640];
    int wrote = snprintf(url, sizeof(url),
                         HAM_API_BASE
                         "/object?apikey=%s&size=%d&page=%d"
                         "&hasimage=1&q=%s"
                         "&%s=%s"
                         "&sort=id&sortorder=asc"
                         "&fields=id,primaryimageurl",
                         api_key, HAM_PAGE_LIMIT, page,
                         encoded_q,
                         axis, encoded_term);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for axis=%s term=%s", axis, term_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (axis=%s term=%.32s)", page, axis, term_id);

    http_fetch_header_t headers[] = {
        { "Accept", "application/json" },
    };
    http_fetch_request_t fr = {
        .url = url,
        .headers = headers,
        .header_count = 1,
        .on_rate_limited = ham_on_rate_limited,
    };
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, response_buf_size, &got, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HAM page %d fetch failed: %s", page, esp_err_to_name(err));
        return err;
    }
    int total_read = (int)got;

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "HAM JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *records = cJSON_GetObjectItem(root, "records");
    if (!cJSON_IsArray(records)) {
        // An empty/over-page response can return `records: []` legitimately.
        // Treat it as success-with-zero so the caller exits the loop cleanly.
        ESP_LOGW(TAG, "HAM response missing 'records' array on page %d", page);
        cJSON_Delete(root);
        *out_count = 0;
        *has_more = false;
        return ESP_OK;
    }

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(records);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *rec = cJSON_GetArrayItem(records, i);
        if (!cJSON_IsObject(rec)) continue;

        const cJSON *primary = cJSON_GetObjectItem(rec, "primaryimageurl");
        if (!cJSON_IsString(primary) || !primary->valuestring[0]) {
            // Should be rare given q=imagepermissionlevel:0, but defensive
            // skip prevents storing entries with no buildable URL.
            continue;
        }

        const char *urn = ham_extract_urn(primary->valuestring);
        if (!urn || !urn[0]) {
            ESP_LOGW(TAG, "HAM primaryimageurl not under NRS host: %.80s",
                     primary->valuestring);
            continue;
        }
        size_t key_len = strlen(urn);
        if (key_len >= sizeof(out_entries[parsed].iiif_key)) {
            ESP_LOGW(TAG, "HAM URN too long (%zu chars), skipping: %.32s", key_len, urn);
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("ham", urn);
        e->kind = 0;
        e->extension = 3;  // jpg — HAM IIIF serves jpg
        e->created_at = now;
        memcpy(e->iiif_key, urn, key_len + 1);
        parsed++;
    }

    // HAM pagination envelope: info.totalrecords + info.pages.
    int total_records = 0;
    int total_pages = 0;
    const cJSON *info = cJSON_GetObjectItem(root, "info");
    if (cJSON_IsObject(info)) {
        const cJSON *tr = cJSON_GetObjectItem(info, "totalrecords");
        const cJSON *tp = cJSON_GetObjectItem(info, "pages");
        if (cJSON_IsNumber(tr)) total_records = (int)cJSON_GetNumberValue(tr);
        if (cJSON_IsNumber(tp)) total_pages   = (int)cJSON_GetNumberValue(tp);
    }
    *has_more = (array_size > 0) && (page < total_pages);
    if (out_total_records) *out_total_records = total_records;

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "HAM page %d: parsed %zu/%d entries (total=%d, pages=%d), has_more=%d",
             page, parsed, array_size, total_records, total_pages, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_ham_refresh_channel(const char *channel_id,
                                              const char *axis,
                                              const char *term_id,
                                              uint32_t channel_offset)
{
    if (!channel_id || !axis || !term_id || !axis[0] || !term_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    // BYOK gate: if no key configured, refresh is a no-op. Persist
    // last_refresh would defeat the purpose of "reactivate when user
    // enters a key", so we deliberately return ESP_OK without touching
    // the channel metadata — the dispatcher treats it as a transient
    // skip and re-evaluates on the next tick. The browse modal hides
    // the HAM entry in this state.
    char api_key[HAM_API_KEY_MAX] = {0};
    config_store_get_ham_api_key(api_key, sizeof(api_key));
    if (api_key[0] == '\0') {
        ESP_LOGI(TAG, "HAM API key not configured; skipping refresh for '%s' "
                 "(enter your key in Settings > Museums)", channel_id);
        return ESP_OK;
    }

    if (art_institution_is_rate_limited("ham")) {
        ESP_LOGW(TAG, "HAM rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(HAM_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(HAM_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        HAM_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(HAM_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;

    // First page fetch — also gives us total_records for the modulo wrap.
    // Worst-case channel_offset + ai_cache_size = 8192, well under HAM's
    // ES window (500 000) per docs/art-institutions/ham-investigation/
    // REPORT.md §Q5, so no partitioning is needed.
    int first_total = 0;
    int starting_page = (int)(channel_offset / HAM_PAGE_LIMIT) + 1;

    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;
    int page = starting_page;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        bool has_more = false;
        int total_records = 0;
        esp_err_t err = ham_fetch_page(axis, term_id, api_key, page,
                                       response_buf, HAM_RESPONSE_BUF_SIZE,
                                       page_entries, HAM_PAGE_LIMIT,
                                       &page_count, &has_more, &total_records);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }
        if (page == starting_page) {
            first_total = total_records;
            // If the user's channel_offset exceeded the term's total, wrap
            // back to the start so the channel doesn't go empty. We re-issue
            // the page=1 fetch in that case.
            if (total_records > 0 && channel_offset >= (uint32_t)total_records && starting_page != 1) {
                uint32_t effective_offset = channel_offset % (uint32_t)total_records;
                int new_page = (int)(effective_offset / HAM_PAGE_LIMIT) + 1;
                if (new_page != starting_page) {
                    ESP_LOGI(TAG, "channel_offset %lu >= total %d; wrapping to page %d",
                             (unsigned long)channel_offset, total_records, new_page);
                    page = new_page;
                    starting_page = new_page;
                    // Don't merge anything from this first probe page.
                    continue;
                }
            }
        }
        (void)first_total;
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
        ESP_LOGI(TAG, "HAM page %d merged: %zu entries (total %zu)",
                 page, page_count, total_fetched);
        download_manager_rescan();

        page++;
        if (!has_more) break;
        // 2 500 req/day per key is generous in absolute terms but a tight
        // refresh of a 4096-entry channel still costs ~41 calls. Be polite.
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    // Orphan eviction only on a complete walk (matches AIC/V&A pattern):
    // a partial walk would misidentify still-listed entries as orphans
    // because Si would be missing them.
    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "ham");
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
        ESP_LOGI(TAG, "HAM refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "HAM refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "HAM refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
