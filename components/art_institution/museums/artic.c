// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/artic.c
 * @brief Art Institute of Chicago (AIC) adapter — refresh + IIIF URL build.
 *
 * Implements docs/art-institutions/finalized-design.md §9.1. The page loop
 * mirrors components/giphy/giphy_refresh.c: each page is merged into the
 * cache under channel_cache_lifecycle_lock() so a concurrent playset
 * switch can't free the cache out from under us between HTTP waits.
 *
 * AIC's listing endpoint returns artworks for a given (axis, term_id)
 * paginated by &page=N. The full axis -> filterField mapping mirrors
 * reference/museum-art/ubi-test/js/adapters/artic.js.
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
#include "version.h"
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

static const char *TAG = "ai_artic";

#define AIC_API_BASE              "https://api.artic.edu/api/v1"
#define AIC_IIIF_BASE             "https://www.artic.edu/iiif/2"
#define AIC_PAGE_LIMIT            100
#define AIC_RESPONSE_BUF_SIZE     (192 * 1024)   // headroom over typical ~80 KB pages
#define AIC_FETCH_MAX_ATTEMPTS    3
static const uint32_t s_fetch_backoff_ms[AIC_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

// External: signal download manager that new entries are mergeable.
extern void download_manager_rescan(void);

// ----- Axis -> filter-field map (mirrors reference adapter) ----------------

typedef struct {
    const char *axis;
    const char *filter_field;
} aic_axis_map_t;

static const aic_axis_map_t AIC_AXES[] = {
    { "departments",     "department_id"    },
    { "classifications", "classification_id" },
    { "subjects",        "subject_id"       },
    { "themes",          "category_ids"     },
    { "galleries",       "gallery_id"       },
    { "artwork-types",   "artwork_type_id"  },
};

static const char *aic_filter_field_for_axis(const char *axis)
{
    if (!axis) return NULL;
    for (size_t i = 0; i < sizeof(AIC_AXES) / sizeof(AIC_AXES[0]); i++) {
        if (strcmp(AIC_AXES[i].axis, axis) == 0) return AIC_AXES[i].filter_field;
    }
    return NULL;
}

// ----- IIIF URL ------------------------------------------------------------

esp_err_t art_institution_artic_build_iiif_url(const institution_channel_entry_t *entry,
                                               int longest_side,
                                               char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (longest_side <= 0) longest_side = 720;

    // AIC's IIIF 2.0 size syntax: !w,h means "fit within w×h, preserve AR".
    int n = snprintf(out, len, AIC_IIIF_BASE "/%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- AIC-User-Agent header value ---------------------------------------

// Built once at first use; AIC requires this header on every request.
static const char *aic_user_agent(void)
{
    static char s_ua[64];
    static bool s_inited = false;
    if (!s_inited) {
        snprintf(s_ua, sizeof(s_ua), "p3a/%s (pub@kury.dev)", FW_VERSION_STRING);
        s_inited = true;
    }
    return s_ua;
}

// ----- One-page fetch ------------------------------------------------------

/**
 * @brief Drain an esp_http_client into the response buffer
 *
 * Returns total bytes read. Caller must null-terminate.
 */
static int drain_body(esp_http_client_handle_t client, char *buf, size_t buf_size)
{
    int total = 0;
    bool read_err = false;
    while (total < (int)buf_size - 1) {
        int n = esp_http_client_read(client, buf + total, buf_size - 1 - total);
        if (n < 0) { read_err = true; break; }
        if (n == 0) break;
        total += n;
    }
    return read_err ? -1 : total;
}

/**
 * @brief Parse a Retry-After header value (seconds form only)
 */
static uint32_t parse_retry_after(const char *value)
{
    if (!value) return 0;
    while (*value == ' ') value++;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || v <= 0) return 0;
    if (v > 3600) v = 3600;  // sanity cap
    return (uint32_t)v;
}

/**
 * @brief Fetch + parse one AIC search page
 *
 * Returns ESP_OK with *out_count set on success (possibly 0). Sets *has_more.
 * Returns ESP_ERR_INVALID_RESPONSE on HTTP 429 (cooldown engaged before return).
 * Returns ESP_ERR_NOT_ALLOWED on 401/403.
 */
static esp_err_t aic_fetch_page(const char *filter_field,
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

    // Encode the term_id (typically alphanumeric / hyphen, but be defensive).
    char encoded_term[128];
    ai_url_encode(term_id, encoded_term, sizeof(encoded_term));

    char url[512];
    int wrote = snprintf(url, sizeof(url),
                         AIC_API_BASE
                         "/artworks/search?query%%5Bterm%%5D%%5B%s%%5D=%s"
                         "&page=%d&limit=%d"
                         "&fields=id,title,image_id,artist_title,date_display",
                         filter_field, encoded_term, page, AIC_PAGE_LIMIT);
    if (wrote < 0 || wrote >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL overflow for filter=%s term=%s", filter_field, term_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching page %d (filter=%s term=%.32s)", page, filter_field, term_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < AIC_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retrying AIC page fetch in %lums (attempt %d/%d)",
                     (unsigned long)s_fetch_backoff_ms[attempt],
                     attempt + 1, AIC_FETCH_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "AIC-User-Agent", aic_user_agent());
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
                cooldown = parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("artic", cooldown);  // 0 -> default 60s
            ESP_LOGW(TAG, "AIC returned 429 (cooldown %us)",
                     (unsigned)(cooldown ? cooldown : 60));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        if (status == 401 || status == 403) {
            ESP_LOGE(TAG, "AIC returned %d (auth issue)", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }

        if (status != 200) {
            ESP_LOGW(TAG, "AIC status %d on page %d", status, page);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;  // retry
        }

        total_read = drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read < 0) continue;
        if (total_read == 0) {
            ESP_LOGW(TAG, "AIC empty body");
            continue;
        }
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "AIC response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        if (content_length > 0 && total_read < (int)content_length) {
            ESP_LOGW(TAG, "AIC truncated: got %d/%lld bytes",
                     total_read, (long long)content_length);
            continue;  // retry
        }

        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) {
        ESP_LOGE(TAG, "AIC page %d fetch failed after %d attempts",
                 page, AIC_FETCH_MAX_ATTEMPTS);
        return ESP_FAIL;
    }

    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) {
        ESP_LOGE(TAG, "AIC JSON parse failed (%d bytes)", total_read);
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "AIC response missing 'data' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t parsed = 0;
    int array_size = cJSON_GetArraySize(data);
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < array_size && parsed < max_entries; i++) {
        const cJSON *art = cJSON_GetArrayItem(data, i);
        if (!cJSON_IsObject(art)) continue;

        const cJSON *image_id = cJSON_GetObjectItem(art, "image_id");
        if (!cJSON_IsString(image_id) || !image_id->valuestring[0]) {
            // No image_id — artwork has no displayable image; skip.
            continue;
        }
        const char *iiif_key = image_id->valuestring;
        size_t key_len = strlen(iiif_key);
        if (key_len >= sizeof(out_entries[parsed].iiif_key)) {
            ESP_LOGW(TAG, "AIC iiif_key too long (%zu chars), skipping", key_len);
            continue;
        }

        institution_channel_entry_t *e = &out_entries[parsed];
        memset(e, 0, sizeof(*e));
        e->post_id = art_institution_compute_post_id("artic", iiif_key);
        e->kind = 0;
        e->extension = 3;  // jpg — matches the shared makapix/giphy encoding
        e->width = 0;
        e->height = 0;
        // date_display is free text ("c. 1981", "1900–1910", ...); not worth
        // a date parser. created_at defaults to wall-clock at refresh time.
        e->created_at = now;
        memcpy(e->iiif_key, iiif_key, key_len + 1);

        parsed++;
    }

    // AIC pagination.total_pages tells us when to stop.
    const cJSON *pagination = cJSON_GetObjectItem(root, "pagination");
    int total_pages = 0;
    int current_page = page;
    if (cJSON_IsObject(pagination)) {
        const cJSON *tp = cJSON_GetObjectItem(pagination, "total_pages");
        const cJSON *cp = cJSON_GetObjectItem(pagination, "current_page");
        if (cJSON_IsNumber(tp)) total_pages = (int)cJSON_GetNumberValue(tp);
        if (cJSON_IsNumber(cp)) current_page = (int)cJSON_GetNumberValue(cp);
    }
    *has_more = (total_pages > 0 && current_page < total_pages && array_size > 0);

    cJSON_Delete(root);

    *out_count = parsed;
    ESP_LOGI(TAG, "AIC page %d: parsed %zu/%d entries, has_more=%d",
             page, parsed, array_size, (int)*has_more);
    return ESP_OK;
}

// ----- Refresh dispatcher --------------------------------------------------

esp_err_t art_institution_artic_refresh_channel(const char *channel_id,
                                                const char *axis,
                                                const char *term_id)
{
    if (!channel_id || !axis || !term_id) return ESP_ERR_INVALID_ARG;

    const char *filter_field = aic_filter_field_for_axis(axis);
    if (!filter_field) {
        ESP_LOGE(TAG, "Unknown AIC axis '%s'", axis);
        return ESP_ERR_INVALID_ARG;
    }

    // Defensive cooldown check (the refresh dispatcher already gated, but
    // a 429 may have arrived between eligibility check and dispatch).
    if (art_institution_is_rate_limited("artic")) {
        ESP_LOGW(TAG, "AIC rate-limited at refresh start, skipping");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Verify the cache exists in the registry; don't keep the pointer
    // across HTTP waits.
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

    char *response_buf = heap_caps_malloc(AIC_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(AIC_RESPONSE_BUF_SIZE);
        if (!response_buf) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    institution_channel_entry_t *page_entries = heap_caps_malloc(
        AIC_PAGE_LIMIT * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(AIC_PAGE_LIMIT * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            ESP_LOGE(TAG, "Failed to allocate page entry buffer");
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
        bool has_more = false;
        esp_err_t err = aic_fetch_page(filter_field, term_id, page,
                                       response_buf, AIC_RESPONSE_BUF_SIZE,
                                       page_entries, AIC_PAGE_LIMIT,
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

        // Re-resolve cache under the lifecycle lock to keep merge atomic
        // wrt concurrent playset switches.
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

        // Track post_ids in Si for the eviction pass.
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
        ESP_LOGI(TAG, "Page %d merged: %zu entries (total %zu)", page, page_count, total_fetched);

        // Start downloads as soon as the first entries land.
        download_manager_rescan();

        page++;
        if (!has_more) break;
        vTaskDelay(pdMS_TO_TICKS(200));  // be nice to AIC's 60-req/min budget
    }

    free(response_buf);
    free(page_entries);

    // Orphan eviction — only when the refresh ran to completion. Partial
    // refreshes leave Ci as-is; the next attempt will redo.
    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "artic");
        }
        channel_cache_lifecycle_unlock();
    }

    // Free Si hash unconditionally.
    {
        ai_si_node_t *node, *tmp;
        HASH_ITER(hh, si_hash, node, tmp) {
            HASH_DEL(si_hash, node);
            free(node);
        }
    }

    // Persist last_refresh on full success only — partial refreshes must
    // not extend the freshness window. Require SNTP-synced clock (matches
    // giphy_refresh.c and makapix_channel_refresh.c).
    if (refresh_completed && sntp_sync_is_synchronized()) {
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

    ESP_LOGI(TAG, "AIC refresh %s for '%s': %zu fetched",
             refresh_completed ? "complete" : "incomplete", channel_id, total_fetched);

    if (refresh_completed && total_fetched > 0) return ESP_OK;
    if (last_err != ESP_OK) return last_err;
    return refresh_completed ? ESP_OK : ESP_FAIL;
}
