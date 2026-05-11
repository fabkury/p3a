// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/rijksmuseum.c
 * @brief Rijksmuseum adapter — refresh + lazy Linked-Art resolution.
 *
 * Implements docs/art-institutions/finalized-design.md §9.2. Rijks
 * paginates differently from AIC (cursor walk over Linked-Art
 * OrderedCollectionPage instead of page=N) and does not expose the
 * IIIF id on the listing itself. The image URL is discovered via a
 * three-hop chain at download time:
 *
 *   HMO -> VisualItem -> DigitalObject -> access_point
 *
 * Each hop is a separate JSON-LD fetch, so we keep the cost off the
 * refresh path: refresh stores HMO ids with extension=0xFF (unresolved
 * sentinel), and a separate resolver loop walks one entry per
 * download_task iteration. Three consecutive failures promote the
 * entry to extension=0xFE (tombstone) so the manager skips it forever
 * until the next refresh re-adds it.
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

static const char *TAG = "ai_rijks";

#define RIJKS_API_BASE            "https://data.rijksmuseum.nl"
#define RIJKS_ID_PREFIX           "https://id.rijksmuseum.nl/"
#define RIJKS_MICRIO_PREFIX       "https://iiif.micr.io/"
#define RIJKS_LIST_RESPONSE_SIZE  (192 * 1024)
#define RIJKS_LD_RESPONSE_SIZE    (96 * 1024)   // single-HMO / VisualItem / DigitalObject
#define RIJKS_FETCH_MAX_ATTEMPTS  3
#define RIJKS_MAX_REDIRECTS       5

static const uint32_t s_fetch_backoff_ms[RIJKS_FETCH_MAX_ATTEMPTS] = { 0, 1000, 3000 };

// External: signal download manager to rescan when new entries land.
extern void download_manager_rescan(void);

// ----- HTTP event handler: capture Location header ------------------------

/**
 * @brief Per-request scratch the event handler writes into
 *
 * esp_http_client_get_header("Location", ...) doesn't reliably return
 * the header even with disable_auto_redirect=true (observed on ESP-IDF
 * v5.5.2: get_header returns ESP_OK with *value=NULL on 3xx responses,
 * regardless of the flag). Snagging the value via the ON_HEADER
 * event callback bypasses whatever the higher-level path is doing —
 * the parser dispatches the event synchronously as it walks the
 * response, so we always see Location regardless of how the client
 * later handles the redirect status.
 */
typedef struct {
    char location[512];
} rijks_fetch_ctx_t;

static esp_err_t rijks_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    rijks_fetch_ctx_t *ctx = (rijks_fetch_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;
    if (evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
        strlcpy(ctx->location, evt->header_value, sizeof(ctx->location));
    }
    return ESP_OK;
}

// ----- HTTP fetch helper --------------------------------------------------

/**
 * @brief GET a JSON-LD document into a caller-provided buffer
 *
 * Returns ESP_OK with the buffer null-terminated. Sets Accept:
 * application/ld+json so Rijks's content negotiation gives us the
 * Linked-Art view consistently across endpoints.
 *
 * Rijks's HMO URLs (https://id.rijksmuseum.nl/{id}) return HTTP 303
 * See Other and the actual JSON-LD lives behind the Location header
 * (typically on data.rijksmuseum.nl). The ESP-IDF HTTP client does
 * NOT auto-follow redirects when using the open/fetch_headers/read
 * pattern, so we walk the redirect chain manually with a small cap.
 *
 * On HTTP 429 engages the per-museum cooldown and returns
 * ESP_ERR_INVALID_RESPONSE; on 401/403 returns ESP_ERR_NOT_ALLOWED.
 */
static esp_err_t rijks_fetch_jsonld(const char *url, char *buf, size_t buf_size,
                                    int *out_total_read)
{
    if (!url || !buf || buf_size == 0 || !out_total_read) return ESP_ERR_INVALID_ARG;
    *out_total_read = 0;

    char current_url[512];
    strlcpy(current_url, url, sizeof(current_url));

    for (int redirect_hop = 0; redirect_hop <= RIJKS_MAX_REDIRECTS; redirect_hop++) {
        rijks_fetch_ctx_t ctx = {0};
        esp_http_client_config_t cfg = {
            .url = current_url,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
            // disable_auto_redirect keeps fetch_headers() honest. Combined
            // with the ON_HEADER event handler below, we capture the
            // Location value reliably regardless of how ESP-IDF would
            // otherwise process the 3xx response.
            .disable_auto_redirect = true,
            .event_handler = rijks_http_event_handler,
            .user_data = &ctx,
        };

        esp_err_t fatal_err = ESP_OK;
        int total_read = 0;
        bool success = false;
        char next_url[512] = "";  // populated if we hit a 3xx with a Location

        for (int attempt = 0; attempt < RIJKS_FETCH_MAX_ATTEMPTS && !success && fatal_err == ESP_OK; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "Retrying %.80s in %lums (attempt %d/%d)",
                         current_url, (unsigned long)s_fetch_backoff_ms[attempt],
                         attempt + 1, RIJKS_FETCH_MAX_ATTEMPTS);
                vTaskDelay(pdMS_TO_TICKS(s_fetch_backoff_ms[attempt]));
                total_read = 0;
            }

            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (!client) continue;
            esp_http_client_set_header(client, "Accept", "application/ld+json");

            esp_err_t open_err = esp_http_client_open(client, 0);
            if (open_err != ESP_OK) {
                ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(open_err));
                esp_http_client_cleanup(client);
                continue;
            }

            esp_http_client_fetch_headers(client);
            int64_t content_length = esp_http_client_get_content_length(client);
            int status = esp_http_client_get_status_code(client);

            if (status == 429) {
                art_institution_set_rate_limited("rijks", 0);
                ESP_LOGW(TAG, "Rijks returned 429");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (status == 401 || status == 403) {
                ESP_LOGW(TAG, "Rijks returned %d for %.80s", status, current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_NOT_ALLOWED;
                break;
            }
            if (status == 404) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_NOT_FOUND;
                break;
            }
            // 301/302/303/307/308 — Location was captured by the event
            // handler during header parsing. We don't use the response
            // body, so the retry loop is short-circuited here.
            if (status >= 300 && status < 400) {
                if (ctx.location[0]) {
                    strlcpy(next_url, ctx.location, sizeof(next_url));
                } else {
                    ESP_LOGW(TAG, "Rijks %d for %.80s but no Location header", status, current_url);
                }
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                success = true;
                total_read = 0;
                break;
            }
            if (status != 200) {
                ESP_LOGW(TAG, "Rijks status %d for %.80s", status, current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                continue;
            }

            bool read_err = false;
            while (total_read < (int)buf_size - 1) {
                int n = esp_http_client_read(client, buf + total_read,
                                              buf_size - 1 - total_read);
                if (n < 0) { read_err = true; break; }
                if (n == 0) break;
                total_read += n;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (read_err) continue;
            if (total_read == 0) continue;
            if (total_read >= (int)buf_size - 1) {
                ESP_LOGE(TAG, "Rijks response truncated at %d bytes for %.80s", total_read, current_url);
                fatal_err = ESP_FAIL;
                break;
            }
            if (content_length > 0 && total_read < (int)content_length) {
                ESP_LOGW(TAG, "Rijks truncated: got %d/%lld bytes", total_read, (long long)content_length);
                continue;
            }
            success = true;
        }

        if (fatal_err != ESP_OK) return fatal_err;
        if (!success) return ESP_FAIL;

        // If the inner loop succeeded via a redirect, advance to the next
        // hop and re-run the retry loop against the new URL.
        if (next_url[0]) {
            ESP_LOGD(TAG, "Rijks redirect %d: %.80s -> %.80s",
                     redirect_hop + 1, current_url, next_url);
            strlcpy(current_url, next_url, sizeof(current_url));
            continue;
        }

        // Real 200 response — body is in buf.
        buf[total_read] = '\0';
        *out_total_read = total_read;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Rijks too many redirects (>%d) starting at %.80s",
             RIJKS_MAX_REDIRECTS, url);
    return ESP_FAIL;
}

// ----- IIIF URL (resolved entry) ------------------------------------------

esp_err_t art_institution_rijks_build_iiif_url(const institution_channel_entry_t *entry,
                                               int longest_side,
                                               char *out, size_t len)
{
    if (!entry || !out || len == 0) return ESP_ERR_INVALID_ARG;
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;
    // Unresolved / tombstone entries have no buildable URL.
    if (entry->extension == 0xFF || entry->extension == 0xFE) return ESP_ERR_INVALID_STATE;
    if (longest_side <= 0) longest_side = 720;

    int n = snprintf(out, len, RIJKS_MICRIO_PREFIX "%s/full/!%d,%d/0/default.jpg",
                     entry->iiif_key, longest_side, longest_side);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

// ----- Refresh -------------------------------------------------------------

// Build the first cursor URL for a Rijks set. The memberOfSetId value is a
// full HTTPS URL that we percent-encode (only the slashes and colon need
// escaping) so the API parses it correctly.
static esp_err_t build_first_cursor(const char *term_id, char *out, size_t len)
{
    if (!term_id || !out || len == 0) return ESP_ERR_INVALID_ARG;
    // Manually encode "https://id.rijksmuseum.nl/{term_id}" since the
    // numeric term_id contains no special chars.
    int n = snprintf(out, len,
                     RIJKS_API_BASE "/search/collection?memberOfSetId="
                     "https%%3A%%2F%%2Fid.rijksmuseum.nl%%2F%s"
                     "&imageAvailable=true",
                     term_id);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t art_institution_rijks_refresh_channel(const char *channel_id,
                                                const char *axis,
                                                const char *term_id)
{
    (void)axis;  // Rijks is axis-less; spec_name is "rijks:set", axis="set"
    if (!channel_id || !term_id || term_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (art_institution_is_rate_limited("rijks")) {
        ESP_LOGW(TAG, "Rijks rate-limited at refresh start, skipping");
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

    char *response_buf = heap_caps_malloc(RIJKS_LIST_RESPONSE_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buf) {
        response_buf = malloc(RIJKS_LIST_RESPONSE_SIZE);
        if (!response_buf) return ESP_ERR_NO_MEM;
    }

    // OrderedCollectionPage typically returns up to 100 items per page.
    // Size the entry buffer for one page at a time.
    const size_t per_page = 100;
    institution_channel_entry_t *page_entries = heap_caps_malloc(
        per_page * sizeof(institution_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(per_page * sizeof(institution_channel_entry_t));
        if (!page_entries) {
            free(response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    char cursor_url[384];
    esp_err_t err = build_first_cursor(term_id, cursor_url, sizeof(cursor_url));
    if (err != ESP_OK) {
        free(response_buf);
        free(page_entries);
        return err;
    }

    ai_si_node_t *si_hash = NULL;
    size_t si_count = 0;
    size_t total_fetched = 0;
    int page_num = 1;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while (total_fetched < cache_size && cursor_url[0] != '\0') {
        ESP_LOGI(TAG, "Fetching page %d (set=%s)", page_num, term_id);

        int total_read = 0;
        err = rijks_fetch_jsonld(cursor_url, response_buf, RIJKS_LIST_RESPONSE_SIZE,
                                  &total_read);
        if (err != ESP_OK) {
            last_err = err;
            refresh_completed = false;
            break;
        }

        cJSON *root = cJSON_Parse(response_buf);
        if (!root) {
            ESP_LOGE(TAG, "Rijks JSON parse failed (%d bytes)", total_read);
            last_err = ESP_FAIL;
            refresh_completed = false;
            break;
        }

        const cJSON *ordered = cJSON_GetObjectItem(root, "orderedItems");
        const cJSON *next    = cJSON_GetObjectItem(root, "next");

        size_t page_count = 0;
        uint32_t now = (uint32_t)time(NULL);
        if (cJSON_IsArray(ordered)) {
            int array_size = cJSON_GetArraySize(ordered);
            for (int i = 0; i < array_size && page_count < per_page; i++) {
                const cJSON *item = cJSON_GetArrayItem(ordered, i);
                if (!cJSON_IsObject(item)) continue;
                const cJSON *id = cJSON_GetObjectItem(item, "id");
                if (!cJSON_IsString(id) || !id->valuestring[0]) continue;

                const char *hmo_url = id->valuestring;
                size_t key_len = strlen(hmo_url);
                if (key_len >= sizeof(page_entries[0].iiif_key)) {
                    ESP_LOGW(TAG, "Rijks HMO URL too long (%zu chars), skipping",
                             key_len);
                    continue;
                }

                institution_channel_entry_t *e = &page_entries[page_count];
                memset(e, 0, sizeof(*e));
                e->post_id = art_institution_compute_post_id("rijks", hmo_url);
                e->kind = 0;
                // Unresolved sentinel — the resolver loop will mutate this
                // to extension=3 (jpg) and replace iiif_key with the
                // micrio short id once the 3-hop walk succeeds.
                e->extension = 0xFF;
                e->created_at = now;
                memcpy(e->iiif_key, hmo_url, key_len + 1);
                e->resolve_fails = 0;
                page_count++;
            }
        }

        // Capture next cursor before destroying the cJSON tree.
        char next_url[384] = "";
        if (cJSON_IsObject(next)) {
            const cJSON *next_id = cJSON_GetObjectItem(next, "id");
            if (cJSON_IsString(next_id) && next_id->valuestring[0]) {
                strlcpy(next_url, next_id->valuestring, sizeof(next_url));
            }
        }
        cJSON_Delete(root);

        if (page_count == 0 && next_url[0] == '\0') {
            ESP_LOGI(TAG, "Rijks: no entries on page %d, done", page_num);
            break;
        }

        if (page_count > 0) {
            // Merge under lifecycle lock — same pattern as AIC and Giphy.
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
                ESP_LOGW(TAG, "Merge failed on page %d: %s", page_num,
                         esp_err_to_name(merge_err));
                last_err = merge_err;
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
            ESP_LOGI(TAG, "Rijks page %d merged: %zu entries (total %zu)",
                     page_num, page_count, total_fetched);
            // Wake the download manager — resolver + downloads can start
            // working their way through what's landed so far.
            download_manager_rescan();
        }

        // Advance cursor.
        if (next_url[0] == '\0') break;
        strlcpy(cursor_url, next_url, sizeof(cursor_url));
        page_num++;

        // Be nice to the API. Rijks doesn't publish a rate-limit number
        // but its endpoint is light; 150 ms between pages keeps us well
        // under any reasonable threshold.
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    free(response_buf);
    free(page_entries);

    bool partial_with_content = (!refresh_completed && total_fetched > 0);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            art_institution_evict_orphans(evict_cache, si_hash, "rijks");
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
        ESP_LOGI(TAG, "Rijks refresh complete for '%s': %zu fetched", channel_id, total_fetched);
        return ESP_OK;
    }
    if (partial_with_content) {
        ESP_LOGW(TAG, "Rijks refresh partial for '%s': %zu fetched, last err: %s (treating as success)",
                 channel_id, total_fetched, esp_err_to_name(last_err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Rijks refresh failed for '%s': %s",
             channel_id, esp_err_to_name(last_err != ESP_OK ? last_err : ESP_FAIL));
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}

// ----- 3-hop Linked Art resolver ------------------------------------------

/**
 * @brief Find the first object in a JSON-LD array whose `id` is a string
 *
 * Many Linked Art collection fields (shows, digitally_shown_by,
 * access_point) are arrays of `{ "id": "...", "type": "..." }` objects.
 * This helper returns the first `id` string in the array, or NULL.
 * Buffer must be caller-owned; the returned cJSON tree is borrowed and
 * remains valid only until cJSON_Delete is called.
 */
static const char *first_id_in_array(const cJSON *array)
{
    if (!cJSON_IsArray(array)) return NULL;
    int n = cJSON_GetArraySize(array);
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(item)) continue;
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(id) && id->valuestring[0]) {
            return id->valuestring;
        }
    }
    return NULL;
}

/**
 * @brief Search a DigitalObject's access_point array for a micrio URL
 *
 * On match, writes the micrio short id (everything between
 * https://iiif.micr.io/ and the next slash) to out. Returns true on
 * success.
 */
static bool extract_micrio_id(const cJSON *access_point, char *out, size_t out_len)
{
    if (!cJSON_IsArray(access_point) || !out || out_len == 0) return false;
    int n = cJSON_GetArraySize(access_point);
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(access_point, i);
        if (!cJSON_IsObject(item)) continue;
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        const char *url = id->valuestring;
        size_t prefix_len = strlen(RIJKS_MICRIO_PREFIX);
        if (strncmp(url, RIJKS_MICRIO_PREFIX, prefix_len) != 0) continue;
        const char *rest = url + prefix_len;
        const char *slash = strchr(rest, '/');
        size_t key_len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (key_len == 0 || key_len >= out_len) continue;
        memcpy(out, rest, key_len);
        out[key_len] = '\0';
        return true;
    }
    return false;
}

esp_err_t art_institution_rijks_resolve_entry(institution_channel_entry_t *entry)
{
    if (!entry) return ESP_ERR_INVALID_ARG;
    if (entry->extension != 0xFF) return ESP_ERR_INVALID_STATE;  // only resolve unresolved
    if (entry->iiif_key[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (art_institution_is_rate_limited("rijks")) {
        ESP_LOGW(TAG, "Rijks rate-limited during resolve, deferring");
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *buf = heap_caps_malloc(RIJKS_LD_RESPONSE_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = malloc(RIJKS_LD_RESPONSE_SIZE);
        if (!buf) return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    char micrio_id[sizeof(entry->iiif_key)] = {0};
    bool found = false;

    // ----- Hop 1: HMO --------------------------------------------------
    int total_read = 0;
    esp_err_t err = rijks_fetch_jsonld(entry->iiif_key, buf, RIJKS_LD_RESPONSE_SIZE,
                                       &total_read);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HMO fetch failed for %.60s: %s",
                 entry->iiif_key, esp_err_to_name(err));
        result = err;
        goto done;
    }
    cJSON *hmo = cJSON_Parse(buf);
    if (!hmo) {
        ESP_LOGW(TAG, "HMO JSON parse failed for %.60s", entry->iiif_key);
        result = ESP_FAIL;
        goto done;
    }

    // ----- Hop 2: VisualItem ------------------------------------------
    const cJSON *shows = cJSON_GetObjectItem(hmo, "shows");
    if (!cJSON_IsArray(shows)) {
        ESP_LOGW(TAG, "HMO missing 'shows' array");
        cJSON_Delete(hmo);
        result = ESP_FAIL;
        goto done;
    }
    int shows_n = cJSON_GetArraySize(shows);
    for (int i = 0; i < shows_n && !found; i++) {
        const cJSON *show_item = cJSON_GetArrayItem(shows, i);
        if (!cJSON_IsObject(show_item)) continue;
        const cJSON *show_id = cJSON_GetObjectItem(show_item, "id");
        if (!cJSON_IsString(show_id) || !show_id->valuestring[0]) continue;

        char visual_url[256];
        strlcpy(visual_url, show_id->valuestring, sizeof(visual_url));

        err = rijks_fetch_jsonld(visual_url, buf, RIJKS_LD_RESPONSE_SIZE, &total_read);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "VisualItem fetch failed for %.60s: %s",
                     visual_url, esp_err_to_name(err));
            continue;
        }
        cJSON *visual = cJSON_Parse(buf);
        if (!visual) continue;

        // ----- Hop 3: DigitalObject -----------------------------------
        const cJSON *digi_arr = cJSON_GetObjectItem(visual, "digitally_shown_by");
        if (cJSON_IsArray(digi_arr)) {
            int digi_n = cJSON_GetArraySize(digi_arr);
            for (int j = 0; j < digi_n && !found; j++) {
                const cJSON *digi_item = cJSON_GetArrayItem(digi_arr, j);
                if (!cJSON_IsObject(digi_item)) continue;
                const cJSON *digi_id = cJSON_GetObjectItem(digi_item, "id");
                if (!cJSON_IsString(digi_id) || !digi_id->valuestring[0]) continue;

                char digi_url[256];
                strlcpy(digi_url, digi_id->valuestring, sizeof(digi_url));

                err = rijks_fetch_jsonld(digi_url, buf, RIJKS_LD_RESPONSE_SIZE, &total_read);
                if (err != ESP_OK) {
                    ESP_LOGD(TAG, "DigitalObject fetch failed for %.60s: %s",
                             digi_url, esp_err_to_name(err));
                    continue;
                }
                cJSON *digital = cJSON_Parse(buf);
                if (!digital) continue;

                const cJSON *ap = cJSON_GetObjectItem(digital, "access_point");
                if (extract_micrio_id(ap, micrio_id, sizeof(micrio_id))) {
                    found = true;
                }
                cJSON_Delete(digital);
            }
        }
        cJSON_Delete(visual);
    }
    cJSON_Delete(hmo);

    if (found) {
        // Mutate the entry: replace HMO URL with micrio short id, extension
        // becomes jpg (matches Rijks's micrio default and the byte encoding
        // shared with makapix/giphy). resolve_fails goes back to 0.
        memset(entry->iiif_key, 0, sizeof(entry->iiif_key));
        strlcpy(entry->iiif_key, micrio_id, sizeof(entry->iiif_key));
        entry->extension = 3;
        entry->resolve_fails = 0;
        result = ESP_OK;
        ESP_LOGI(TAG, "Resolved Rijks HMO -> micrio id %s", micrio_id);
    } else {
        ESP_LOGW(TAG, "No micrio access_point found in Linked Art chain for %.60s",
                 entry->iiif_key);
        result = ESP_ERR_NOT_FOUND;
    }

done:
    free(buf);
    // first_id_in_array is unused right now but kept around for the M3
    // settings-help path that may want to surface "now showing" metadata.
    (void)first_id_in_array;
    return result;
}
