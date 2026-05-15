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
                cooldown = ai_parse_retry_after(retry_after);
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
            // Logged at WARN: AIC has been observed returning 403 on deeper
            // pages of certain large-result-set queries (e.g. Painting past
            // page ~10), independent of the documented offset cap. The
            // caller decides whether to fail the refresh: if pages 1..N-1
            // succeeded, treat this as partial success rather than rendering
            // a hard error.
            ESP_LOGW(TAG, "AIC returned %d on page %d (filter=%s term=%.32s)",
                     status, page, filter_field, term_id);
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

        total_read = ai_drain_body(client, response_buf, response_buf_size);
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

// Forward decl for the offset-aware partitioning path (defined below).
static esp_err_t aic_refresh_partitioned(const char *channel_id,
                                         const char *filter_field,
                                         const char *term_id,
                                         uint32_t channel_offset,
                                         uint32_t cache_size,
                                         char *response_buf,
                                         institution_channel_entry_t *page_entries);

esp_err_t art_institution_artic_refresh_channel(const char *channel_id,
                                                const char *axis,
                                                const char *term_id,
                                                uint32_t channel_offset)
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

    // For non-zero offsets we cannot serve the request with the public GET
    // path (it caps at from+size ≤ 1000). Switch to the POST DSL bool+range
    // partitioning route described in docs/art-institutions/offset-tests/
    // REPORT.md §1.7. The partitioned path does its own refresh + merge +
    // eviction; this function returns the partitioned result directly.
    if (channel_offset > 0) {
        esp_err_t pret = aic_refresh_partitioned(channel_id, filter_field, term_id,
                                                 channel_offset, cache_size,
                                                 response_buf, page_entries);
        free(response_buf);
        free(page_entries);
        return pret;
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

    // A partial refresh that still produced entries (typically because
    // AIC returned 403 on a deep page after several pages of success) is
    // treated as a soft success: the dispatcher must not render a hard
    // error and we don't want to retry immediately, but we also must not
    // run orphan eviction (entries that simply hadn't been re-fetched
    // would be misidentified as evicted).
    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    // Orphan eviction — only on a complete walk through the listing.
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

    // Persist last_refresh whenever we got any content — partial refreshes
    // count as fresh enough so the dispatcher doesn't retry every tick.
    // Skip when total_fetched == 0 so a fully-failed refresh retries on
    // the next dispatcher cycle. Require SNTP-synced clock (matches
    // giphy_refresh.c and makapix_channel_refresh.c).
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
        ESP_LOGI(TAG, "AIC refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "AIC refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "AIC refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}

// ====================================================================
// AIC bool+range partitioning (used when channel_offset > 0)
// ====================================================================
//
// Why this exists: AIC's public search endpoint hard-caps `from + size`
// at 1000 for unauthenticated callers (see docs/art-institutions/
// offset-tests/REPORT.md §1.5 for the source-of-truth on this). The
// simple GET path above tops out at offset 900. To honor a user-set
// channel_offset above that we partition the term's ID space into
// <1000-record buckets with POST DSL bool+range filters and walk one
// bucket at a time, each bucket internally respecting the 1000-cap.
//
// Buckets are discovered at refresh-time (the user chose
// "Recomputed every refresh, never persisted" in design Q&A) and
// stored in a fixed-size array on the refresh task's stack.

#define AIC_PARTITION_MAX_BUCKETS 64
#define AIC_PARTITION_MAX_ID      1000000  // AIC artwork IDs cluster below 330k as of 2026; 1M gives ample headroom against future growth
#define AIC_PARTITION_BUCKET_CAP  1000     // The actual server-enforced from+size limit
#define AIC_PARTITION_MIN_RANGE   100      // Stop subdividing once the ID window is this small; let oversized leaves 403 if they happen

typedef struct {
    int32_t lo;       // inclusive
    int32_t hi;       // exclusive
    int32_t count;    // -1 = uncounted, 0..1000 = ready, >1000 = needs split
} aic_bucket_t;

/**
 * @brief Issue a POST bool+range query and parse pagination.total + entries
 *
 * If `out_entries` is NULL or `max_entries` is 0, only the count is parsed
 * (used by bucket discovery; we still pass size=0 to skip transferring the
 * hits array). When `out_entries` is non-NULL the function reads the entries
 * into out_entries[0..min(max_entries, hits)) and reports them via out_count.
 *
 * Returns: ESP_OK on success, ESP_ERR_INVALID_RESPONSE on 429,
 * ESP_ERR_NOT_ALLOWED on 401/403, ESP_FAIL on transport/parse error.
 */
static esp_err_t aic_post_dsl(const char *filter_field,
                              const char *term_id,
                              int32_t range_lo,
                              int32_t range_hi,
                              int from,
                              int size,
                              char *response_buf,
                              size_t response_buf_size,
                              institution_channel_entry_t *out_entries,
                              size_t max_entries,
                              int *out_total,
                              size_t *out_count)
{
    if (out_total) *out_total = 0;
    if (out_count) *out_count = 0;

    char body[512];
    int blen;
    // term_id arrives as a decimal string for all AIC axes that we support
    // (every filter_field in AIC_AXES is *_id). Embed it as a JSON number so
    // ES treats the filter as a numeric term. category_ids is array-typed on
    // the server side but a `term` query against an array field matches when
    // any element equals the value.
    if (out_entries && max_entries > 0) {
        blen = snprintf(body, sizeof(body),
            "{\"query\":{\"bool\":{"
                "\"must\":[{\"term\":{\"%s\":%s}}],"
                "\"filter\":[{\"range\":{\"id\":{\"gte\":%ld,\"lt\":%ld}}}]}},"
            "\"sort\":[{\"id\":\"asc\"}],"
            "\"from\":%d,\"size\":%d,"
            "\"fields\":[\"id\",\"title\",\"image_id\",\"artist_title\",\"date_display\"]}",
            filter_field, term_id, (long)range_lo, (long)range_hi, from, size);
    } else {
        // Count-only probe: smallest possible response body, size=0.
        blen = snprintf(body, sizeof(body),
            "{\"query\":{\"bool\":{"
                "\"must\":[{\"term\":{\"%s\":%s}}],"
                "\"filter\":[{\"range\":{\"id\":{\"gte\":%ld,\"lt\":%ld}}}]}},"
            "\"from\":0,\"size\":0,"
            "\"fields\":[\"id\"]}",
            filter_field, term_id, (long)range_lo, (long)range_hi);
    }
    if (blen < 0 || blen >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "POST body overflow");
        return ESP_FAIL;
    }

    esp_http_client_config_t cfg = {
        .url = AIC_API_BASE "/artworks/search",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_err_t fatal_err = ESP_OK;
    int total_read = 0;
    bool success = false;

    for (int attempt = 0; attempt < AIC_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
            total_read = 0;
        }

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        esp_http_client_set_header(client, "AIC-User-Agent", aic_user_agent());
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Content-Type", "application/json");

        // open(blen) declares the Content-Length; the body itself is sent
        // via the explicit write() below. (set_post_field is only consulted
        // by perform(); we run the request manually so we can drain the
        // body into our PSRAM response_buf.)
        esp_err_t err = esp_http_client_open(client, blen);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            continue;
        }
        if (esp_http_client_write(client, body, blen) != blen) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 429) {
            char *retry_after = NULL;
            uint32_t cooldown = 0;
            if (esp_http_client_get_header(client, "Retry-After", &retry_after) == ESP_OK) {
                cooldown = ai_parse_retry_after(retry_after);
            }
            art_institution_set_rate_limited("artic", cooldown);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "AIC POST DSL returned %d (range=[%ld,%ld) from=%d size=%d)",
                     status, (long)range_lo, (long)range_hi, from, size);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fatal_err = ESP_ERR_NOT_ALLOWED;
            break;
        }
        if (status != 200) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        total_read = ai_drain_body(client, response_buf, response_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total_read <= 0) continue;
        if (total_read >= (int)response_buf_size - 1) {
            ESP_LOGE(TAG, "AIC POST DSL response truncated at %d bytes", total_read);
            fatal_err = ESP_FAIL;
            break;
        }
        success = true;
    }

    if (fatal_err != ESP_OK) return fatal_err;
    if (!success) return ESP_FAIL;

    response_buf[total_read] = '\0';

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) return ESP_FAIL;

    const cJSON *pagination = cJSON_GetObjectItem(root, "pagination");
    if (cJSON_IsObject(pagination)) {
        const cJSON *t = cJSON_GetObjectItem(pagination, "total");
        if (cJSON_IsNumber(t) && out_total) {
            *out_total = (int)cJSON_GetNumberValue(t);
        }
    }

    if (out_entries && max_entries > 0) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsArray(data)) {
            size_t parsed = 0;
            uint32_t now = (uint32_t)time(NULL);
            int n = cJSON_GetArraySize(data);
            for (int i = 0; i < n && parsed < max_entries; i++) {
                const cJSON *art = cJSON_GetArrayItem(data, i);
                if (!cJSON_IsObject(art)) continue;
                const cJSON *image_id = cJSON_GetObjectItem(art, "image_id");
                if (!cJSON_IsString(image_id) || !image_id->valuestring[0]) continue;
                const char *iiif_key = image_id->valuestring;
                size_t key_len = strlen(iiif_key);
                if (key_len >= sizeof(out_entries[parsed].iiif_key)) continue;
                institution_channel_entry_t *e = &out_entries[parsed];
                memset(e, 0, sizeof(*e));
                e->post_id = art_institution_compute_post_id("artic", iiif_key);
                e->kind = 0;
                e->extension = 3;
                e->created_at = now;
                memcpy(e->iiif_key, iiif_key, key_len + 1);
                parsed++;
            }
            if (out_count) *out_count = parsed;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief Recursively split [lo, hi) until every bucket has count ≤ 1000
 *
 * Writes discovered buckets into `buckets` (capacity `cap`). On overflow
 * returns ESP_ERR_NO_MEM but leaves the partial array intact — caller may
 * still walk the buckets it received as a degraded mode (some entries may
 * never be reachable). Empty ranges (count=0) are omitted.
 */
static esp_err_t aic_discover_buckets(const char *filter_field,
                                      const char *term_id,
                                      int32_t lo, int32_t hi,
                                      aic_bucket_t *buckets, size_t cap,
                                      size_t *count_out,
                                      char *response_buf,
                                      size_t response_buf_size)
{
    if (*count_out >= cap) return ESP_ERR_NO_MEM;
    if (lo >= hi) return ESP_OK;

    int total = 0;
    esp_err_t err = aic_post_dsl(filter_field, term_id, lo, hi, 0, 0,
                                 response_buf, response_buf_size,
                                 NULL, 0, &total, NULL);
    if (err != ESP_OK) return err;

    if (total == 0) {
        return ESP_OK;  // empty range; skip
    }

    if (total <= AIC_PARTITION_BUCKET_CAP || (hi - lo) <= AIC_PARTITION_MIN_RANGE) {
        if (*count_out >= cap) return ESP_ERR_NO_MEM;
        buckets[*count_out].lo = lo;
        buckets[*count_out].hi = hi;
        buckets[*count_out].count = total;
        (*count_out)++;
        ESP_LOGI(TAG, "Bucket [%ld,%ld) count=%d", (long)lo, (long)hi, total);
        return ESP_OK;
    }

    // Split — politeness delay so discovery doesn't burn through the
    // ~60 req/min rate budget too fast on big facets.
    vTaskDelay(pdMS_TO_TICKS(150));
    int32_t mid = lo + (hi - lo) / 2;
    err = aic_discover_buckets(filter_field, term_id, lo, mid,
                               buckets, cap, count_out,
                               response_buf, response_buf_size);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) return err;
    if (err == ESP_ERR_NO_MEM) return err;
    return aic_discover_buckets(filter_field, term_id, mid, hi,
                                buckets, cap, count_out,
                                response_buf, response_buf_size);
}

static esp_err_t aic_refresh_partitioned(const char *channel_id,
                                         const char *filter_field,
                                         const char *term_id,
                                         uint32_t channel_offset,
                                         uint32_t cache_size,
                                         char *response_buf,
                                         institution_channel_entry_t *page_entries)
{
    aic_bucket_t buckets[AIC_PARTITION_MAX_BUCKETS];
    size_t bucket_count = 0;

    ESP_LOGI(TAG, "AIC partitioned refresh: filter=%s term=%s offset=%lu cache=%lu",
             filter_field, term_id, (unsigned long)channel_offset, (unsigned long)cache_size);

    esp_err_t err = aic_discover_buckets(filter_field, term_id,
                                         0, AIC_PARTITION_MAX_ID,
                                         buckets, AIC_PARTITION_MAX_BUCKETS,
                                         &bucket_count,
                                         response_buf, AIC_RESPONSE_BUF_SIZE);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "Bucket discovery failed: %s", esp_err_to_name(err));
        return err;
    }
    if (bucket_count == 0) {
        ESP_LOGI(TAG, "AIC partitioned refresh: term has 0 records; nothing to fetch");
        return ESP_OK;
    }

    // Compute total record count across all buckets; modulo-wrap the
    // user-supplied offset against it.
    int64_t total_records = 0;
    for (size_t i = 0; i < bucket_count; i++) {
        total_records += buckets[i].count;
    }
    uint32_t effective_offset = (total_records > 0)
        ? (uint32_t)((uint64_t)channel_offset % (uint64_t)total_records)
        : 0;

    ESP_LOGI(TAG, "AIC partitioned: %zu buckets, total=%lld, effective_offset=%lu",
             bucket_count, (long long)total_records, (unsigned long)effective_offset);

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    size_t entries_walked = 0;   // count of entries we've moved past (offset progress)
    bool refresh_completed = true;
    esp_err_t last_err = ESP_OK;

    for (size_t bi = 0; bi < bucket_count && total_fetched < cache_size; bi++) {
        const aic_bucket_t *b = &buckets[bi];
        int bucket_count_int = b->count;

        // Fast-forward past buckets that lie entirely below the offset.
        if (entries_walked + (size_t)bucket_count_int <= effective_offset) {
            entries_walked += (size_t)bucket_count_int;
            continue;
        }

        // Within this bucket, fast-forward over already-skipped entries.
        size_t within_bucket_skip = 0;
        if (entries_walked < effective_offset) {
            within_bucket_skip = effective_offset - entries_walked;
            entries_walked = effective_offset;
        }

        int from = (int)within_bucket_skip;
        while (from < bucket_count_int && total_fetched < cache_size) {
            int remaining = bucket_count_int - from;
            int size_req = AIC_PAGE_LIMIT;
            if (size_req > remaining) size_req = remaining;
            // Enforce the per-query cap: from + size ≤ 1000.
            int budget = AIC_PARTITION_BUCKET_CAP - from;
            if (budget <= 0) break;
            if (size_req > budget) size_req = budget;

            size_t page_count = 0;
            err = aic_post_dsl(filter_field, term_id, b->lo, b->hi,
                               from, size_req,
                               response_buf, AIC_RESPONSE_BUF_SIZE,
                               page_entries, AIC_PAGE_LIMIT,
                               NULL, &page_count);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "POST DSL fetch failed at bucket [%ld,%ld) from=%d: %s",
                         (long)b->lo, (long)b->hi, from, esp_err_to_name(err));
                last_err = err;
                refresh_completed = false;
                goto done;
            }
            if (page_count == 0) {
                ESP_LOGI(TAG, "Empty page at bucket [%ld,%ld) from=%d, advancing",
                         (long)b->lo, (long)b->hi, from);
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
                goto done;
            }
            if (merge_err != ESP_OK) {
                last_err = merge_err;
                refresh_completed = false;
                goto done;
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
            entries_walked += page_count;
            ESP_LOGI(TAG, "Bucket [%ld,%ld) from=%d: merged %zu (total %zu/%lu)",
                     (long)b->lo, (long)b->hi, from, page_count, total_fetched,
                     (unsigned long)cache_size);
            download_manager_rescan();

            from += (int)page_count;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

done: ;
    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "artic");
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
        channel_metadata_save(channel_id, channels_path, &meta);
    }

    if (refresh_completed) {
        ESP_LOGI(TAG, "AIC partitioned refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "AIC partitioned refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
