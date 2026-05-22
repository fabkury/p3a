// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/smithsonian.c
 * @brief Smithsonian Open Access adapter — refresh + IIIF URL build.
 *
 * One axis: `unit` — the channel's `term_id` is a Smithsonian unit code
 * (SAAM, NPG, CHNDM, NMAAHC, HMSG, NMAfA in v1; see DEFERRED.md for the
 * exclusions and the full unit roster). The firmware accepts any unit code
 * — the v1 curation lives in the web UI; firmware just paginates whatever
 * it is asked to.
 *
 * Search endpoint: https://api.si.edu/openaccess/api/v1.0/search
 *   - Pagination: `start` + `rows` (true offset, range-friendly)
 *   - Filter: `q=unit_code:{X} AND online_visual_material:true` (Solr-style)
 *     Note: probe D in reference/museum-art/source/smithsonian/output/
 *     report.md showed `usage:CC0` is NOT a Solr-indexed field — using it
 *     as an extra AND clause returns zero hits. Rights are checked per-item
 *     only if/when v2 grows strict-CC0 filtering.
 *
 * Image endpoint: https://ids.si.edu/ids/iiif/{idsId}/full/!{N},{N}/0/default.jpg
 *   - The `idsId` is extracted from
 *     `content.descriptiveNonRepeating.online_media.media[*].idsId`.
 *     The media field is sometimes a single object, sometimes a list of
 *     objects — extractor handles both shapes.
 *   - Stored as-is in `iiif_key` (typically 18-30 chars; well under the
 *     48-byte slot).
 *
 * BYOK: Smithsonian requires an api.data.gov key. The user enters their
 * personal key in settings.html and it is stored in NVS under `si_api_key`.
 * When the key is empty, refresh is a no-op (ESP_LOGI + return ESP_OK so
 * the dispatcher waits the full ai_refresh_sec window before retrying).
 * api.data.gov DEMO_KEY is intentionally rate-capped at ~30 req/hour/IP —
 * not enough for a real channel refresh, so users must register their own.
 *
 * Phase A research artifacts live at
 * reference/museum-art/source/smithsonian/ (run.py, output/report.md, and
 * DEFERRED.md for excluded options).
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

static const char *TAG = "ai_si";

#define SI_API_BASE              "https://api.si.edu/openaccess/api/v1.0"
#define SI_IIIF_PREFIX           "https://ids.si.edu/ids/iiif/"
// Page size is intentionally smaller than HAM's 100. Smithsonian records
// are deeply nested (freetext + indexedStructured + descriptiveNonRepeating
// blocks); SAAM hit > 512KB at rows=100 on the first refresh. 50 records
// comfortably fits the 1 MB buffer below even for the most verbose units
// (NMAAHC's provenance text and CHNDM's design metadata can push individual
// records over 10KB).
#define SI_PAGE_LIMIT            50
#define SI_RESPONSE_BUF_SIZE     (1024 * 1024)
#define SI_FETCH_MAX_ATTEMPTS    3
#define SI_API_KEY_MAX           64

static const uint32_t s_fetch_backoff_ms[SI_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

extern void download_manager_rescan(void);

// Both api.si.edu (search) and ids.si.edu (image) sit behind the same
// F5 BIG-IP ASM WAF and reject requests with empty/default User-Agent
// (HTTP 200 with "Request Rejected" HTML body). The shared ai_user_agent()
// in museums/common.c carries the required header on both paths.

// ----- IIIF URL ------------------------------------------------------------

esp_err_t art_institution_si_build_iiif_url(const institution_channel_entry_t *entry,
                                            int longest_side,
                                            char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, SI_IIIF_PREFIX "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Extract idsId from a Smithsonian record -----------------------------

/**
 * @brief Walk `content.descriptiveNonRepeating.online_media.media[*].idsId`
 *
 * The `media` field is either an object (when the record has exactly one
 * media file) or an array of objects (when it has more). Returns the first
 * non-empty `idsId` string found, or NULL if the record has no usable IDs.
 *
 * The returned pointer aliases into the cJSON node tree — valid only until
 * the parent cJSON is deleted. Callers must copy before that point.
 */
static const char *si_extract_ids_id(const cJSON *rec)
{
    if (!rec) return NULL;
    const cJSON *content = cJSON_GetObjectItem(rec, "content");
    if (!cJSON_IsObject(content)) return NULL;
    const cJSON *desc = cJSON_GetObjectItem(content, "descriptiveNonRepeating");
    if (!cJSON_IsObject(desc)) return NULL;
    const cJSON *om = cJSON_GetObjectItem(desc, "online_media");
    if (!cJSON_IsObject(om)) return NULL;
    const cJSON *media = cJSON_GetObjectItem(om, "media");
    if (!media) return NULL;

    if (cJSON_IsArray(media)) {
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, media) {
            if (!cJSON_IsObject(m)) continue;
            const cJSON *id_node = cJSON_GetObjectItem(m, "idsId");
            if (cJSON_IsString(id_node) && id_node->valuestring && id_node->valuestring[0]) {
                return id_node->valuestring;
            }
        }
    } else if (cJSON_IsObject(media)) {
        const cJSON *id_node = cJSON_GetObjectItem(media, "idsId");
        if (cJSON_IsString(id_node) && id_node->valuestring && id_node->valuestring[0]) {
            return id_node->valuestring;
        }
    }
    return NULL;
}

// ----- One-page fetch ------------------------------------------------------

/**
 * @brief Fetch + parse one Smithsonian /search page
 *
 * Builds the URL with `api_key`, `q` (URL-encoded unit_code + online media
 * filter), `start`, and `rows`. Walks the `response.rows[]` array, extracts
 * the idsId per record, and stores ready-to-use cache entries.
 *
 * Reports total `rowCount` so the caller can compute `has_more` and decide
 * whether to wrap modulo the unit size on a deep `channel_offset`.
 */
static esp_err_t si_fetch_page(const char *unit_code,
                               const char *api_key,
                               int start,
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

    // Build the q expression first, then percent-encode it once. The Solr
    // query has spaces, colons and the keyword AND — all need encoding.
    char q_expr[256];
    int qw = snprintf(q_expr, sizeof(q_expr),
                      "unit_code:%s AND online_visual_material:true", unit_code);
    if (qw < 0 || qw >= (int)sizeof(q_expr)) {
        ESP_LOGE(TAG, "q expression overflow for unit_code='%s'", unit_code);
        return ESP_FAIL;
    }

    char encoded_q[768];  // 3x expansion worst case
    ai_url_encode(q_expr, encoded_q, sizeof(encoded_q));

    char url[1024];
    int wrote = snprintf(url, sizeof(url),
                         SI_API_BASE
                         "/search?api_key=%s&q=%s&start=%d&rows=%d",
                         api_key, encoded_q, start, SI_PAGE_LIMIT);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for unit_code='%s' start=%d", unit_code, start);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching start=%d (unit_code=%.16s)", start, unit_code);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,  // Smithsonian search latency can run high
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < SI_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying SI page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, SI_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "User-Agent", ai_user_agent());
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
            art_institution_set_rate_limited("si", cooldown);
            ESP_LOGW(TAG, "SI returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            // api.data.gov returns 403 for an invalid/revoked key; treat
            // both as "key problem" so the dispatcher surfaces it and the
            // user re-enters the key in Settings.
            ESP_LOGW(TAG, "SI returned %d on start=%d — API key invalid?", status, start);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "SI status %d on start=%d", status, start);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;  // retry
        }

        total_read = ai_drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read < 0) continue;
        if (total_read == 0) continue;
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "SI response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "SI truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "SI start=%d fetch failed after %d attempts",
                 start, SI_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }
    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        // Diagnostic: log URL (with key redacted) and the body so we can
        // tell whether the server returned an error envelope, an HTML page,
        // or something else entirely.
        const char *key_pos = strstr(url, "api_key=");
        const char *q_pos = strstr(url, "&q=");
        ESP_LOGE(TAG, "SI JSON parse failed (%d bytes)", total_read);
        if (key_pos && q_pos && q_pos > key_pos) {
            ESP_LOGE(TAG, "URL: %.*s[REDACTED]%s",
                     (int)(key_pos - url + 8), url, q_pos);
        } else {
            ESP_LOGE(TAG, "URL: %s", url);
        }
        ESP_LOGE(TAG, "Body: %.512s", response_buf);
        return ESP_FAIL;
    }

    const cJSON *response = cJSON_GetObjectItem(root, "response");
    if (!cJSON_IsObject(response)) {
        ESP_LOGE(TAG, "SI response missing 'response' object on start=%d", start);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *rows = cJSON_GetObjectItem(response, "rows");
    if (!cJSON_IsArray(rows)) {
        // An over-page request can return rows: null legitimately. Treat
        // as success-with-zero so the caller exits the loop cleanly.
        ESP_LOGI(TAG, "SI 'rows' missing or not an array on start=%d (likely past end)", start);
        cJSON_Delete(root);
        *out_count = 0;
        *has_more = false;
        return ESP_OK;
    }

    int total_records = 0;
    const cJSON *rc = cJSON_GetObjectItem(response, "rowCount");
    if (cJSON_IsNumber(rc)) total_records = (int)cJSON_GetNumberValue(rc);
    if (out_total_records) *out_total_records = total_records;

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(rows);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *rec = cJSON_GetArrayItem(rows, i);
        if (!cJSON_IsObject(rec)) continue;

        const char *ids_id = si_extract_ids_id(rec);
        if (!ids_id || !ids_id[0]) {
            // Record has no online media → skip. Phase A measured 100%
            // idsId presence on `online_visual_material:true` hits for
            // wired units; this is a defensive skip rather than an
            // expected branch.
            continue;
        }
        size_t key_len = strlen(ids_id);
        if (key_len >= sizeof(out_entries[parsed].iiif_key)) {
            ESP_LOGW(TAG, "SI idsId too long (%zu chars), skipping: %.32s", key_len, ids_id);
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("si", ids_id);
        e->kind = 0;
        e->extension = 3;  // jpg — IDS serves jpg via IIIF
        e->created_at = now;
        memcpy(e->iiif_key, ids_id, key_len + 1);
        parsed++;
    }

    // Smithsonian doesn't return a "pages" envelope; compute has_more from
    // rowCount and the position we asked for.
    *has_more = (array_size > 0) && ((start + array_size) < total_records);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "SI start=%d: parsed %zu/%d entries (total=%d), has_more=%d",
             start, parsed, array_size, total_records, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_si_refresh_channel(const char *channel_id,
                                             const char *axis,
                                             const char *term_id,
                                             uint32_t channel_offset)
{
    if (!channel_id || !axis || !term_id || !axis[0] || !term_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    // v1 ships one axis (`unit`); reject anything else so a future axis
    // can't silently misbehave.
    if (strcmp(axis, "unit") != 0) {
        ESP_LOGW(TAG, "SI: unknown axis '%s' (only 'unit' supported in v1)", axis);
        return ESP_ERR_INVALID_ARG;
    }

    // BYOK gate: matches HAM. No key → log + return ESP_OK without touching
    // channel metadata, so the dispatcher re-evaluates on the next tick and
    // refresh "comes alive" the moment the user saves a key.
    char api_key[SI_API_KEY_MAX] = {0};
    config_store_get_si_api_key(api_key, sizeof(api_key));
    if (api_key[0] == '\0') {
        ESP_LOGI(TAG, "SI API key not configured; skipping refresh for '%s' "
                 "(enter your api.data.gov key in Settings > Museums)", channel_id);
        return ESP_OK;
    }

    if (art_institution_is_rate_limited("si")) {
        ESP_LOGW(TAG, "SI rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(SI_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(SI_RESPONSE_BUF_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        SI_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(SI_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;

    // Align channel_offset to a page boundary so we always fetch full pages.
    // Same idea as HAM's starting_page calculation.
    int starting_start = (int)((channel_offset / SI_PAGE_LIMIT) * SI_PAGE_LIMIT);
    int start = starting_start;

    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size) {
        size_t page_count = 0;
        bool has_more = false;
        int total_records = 0;
        esp_err_t err = si_fetch_page(term_id, api_key, start,
                                      response_buf, SI_RESPONSE_BUF_SIZE,
                                      page_entries, SI_PAGE_LIMIT,
                                      &page_count, &has_more, &total_records);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }

        // On the first page, if the user's channel_offset exceeded the
        // unit's total, wrap modulo the total so the channel doesn't go
        // empty. Re-issue the fetch at the wrapped offset.
        if (start == starting_start && total_records > 0 &&
            channel_offset >= (uint32_t)total_records && starting_start != 0) {
            uint32_t effective_offset = channel_offset % (uint32_t)total_records;
            int new_start = (int)((effective_offset / SI_PAGE_LIMIT) * SI_PAGE_LIMIT);
            if (new_start != starting_start) {
                ESP_LOGI(TAG, "channel_offset %lu >= total %d; wrapping to start=%d",
                         (unsigned long)channel_offset, total_records, new_start);
                start = new_start;
                starting_start = new_start;
                continue;  // skip merging this probe page
            }
        }

        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries at start=%d, done", start);
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
            ESP_LOGW(TAG, "Merge failed at start=%d: %s", start, esp_err_to_name(merge_err));
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
        ESP_LOGI(TAG, "SI start=%d merged: %zu entries (total %zu)",
                 start, page_count, total_fetched);
        download_manager_rescan();

        start += SI_PAGE_LIMIT;
        if (!has_more) break;
        // Polite pacing — api.data.gov's default registered-key quota is
        // 1000 req/hour; a 4096-entry refresh costs ~82 calls at the 50/
        // page rate so we are well clear of throttling, but a 200 ms
        // inter-page sleep keeps us from spiking the server.
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    // Orphan eviction only on a complete walk (matches HAM/AIC/V&A
    // pattern): a partial walk would misidentify still-listed entries
    // as orphans because Si would be missing them.
    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "si");
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
        ESP_LOGI(TAG, "SI refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "SI refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "SI refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
