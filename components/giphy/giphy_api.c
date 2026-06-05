// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file giphy_api.c
 * @brief Giphy API client - fetches trending and search GIFs via HTTP
 */

#include "giphy.h"
#include "giphy_types.h"
#include "http_fetch.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "giphy_api";

// ============================================================================
// Process-wide rate-limit cooldown
// ============================================================================
// Beta keys get 100 requests/hour; once we see a 429 the whole API key is
// useless until the bucket resets. We don't know where in the server's hourly
// window we are, so default to 10 min and accept the chance of another 429
// rather than block for a full hour after the bucket has already reset.
#define GIPHY_DEFAULT_COOLDOWN_SEC 600

static int64_t s_cooldown_until_us = 0;

void giphy_set_rate_limited(uint32_t cooldown_sec)
{
    if (cooldown_sec == 0) cooldown_sec = GIPHY_DEFAULT_COOLDOWN_SEC;
    int64_t until = esp_timer_get_time() + (int64_t)cooldown_sec * 1000000LL;
    if (until > s_cooldown_until_us) {
        s_cooldown_until_us = until;
        ESP_LOGW(TAG, "Giphy rate-limit cooldown engaged: %lus",
                 (unsigned long)cooldown_sec);
    }
}

bool giphy_is_rate_limited(void)
{
    return esp_timer_get_time() < s_cooldown_until_us;
}

uint32_t giphy_cooldown_remaining_sec(void)
{
    int64_t now = esp_timer_get_time();
    if (now >= s_cooldown_until_us) return 0;
    return (uint32_t)((s_cooldown_until_us - now + 999999) / 1000000);
}

/**
 * @brief Read a Giphy rendition dimension (served as string or number)
 *
 * Giphy is inconsistent: most rendition dims arrive as strings ("480"),
 * but some GIFs report them as bare JSON numbers. Accept both.
 */
static uint16_t json_dim_u16(const cJSON *v)
{
    if (cJSON_IsString(v) && v->valuestring[0]) {
        int n = atoi(v->valuestring);
        return (n > 0 && n <= UINT16_MAX) ? (uint16_t)n : 0;
    }
    if (cJSON_IsNumber(v) && v->valueint > 0 && v->valueint <= UINT16_MAX) {
        return (uint16_t)v->valueint;
    }
    return 0;
}

/**
 * @brief Rendition-override flag from a downsized_medium url
 *
 * Giphy materializes a dedicated giphy-downsized-medium.gif for only some
 * GIFs; for the rest downsized_medium is a passthrough whose url points
 * straight at giphy.gif (and the guessed -downsized-medium filename 404s
 * on the CDN). The filename inside the API-provided url is therefore the
 * only authority on which file to fetch from i.giphy.com.
 *
 * @return 1 = dedicated giphy-downsized-medium.gif exists,
 *         2 = passthrough to giphy.gif,
 *         0 = unrecognized filename (caller must not override)
 */
static uint8_t dm_flag_from_url(const cJSON *url)
{
    if (!cJSON_IsString(url) || !url->valuestring[0]) return 0;

    // Filename = segment after the last '/' before the query string
    const char *s = url->valuestring;
    const char *end = strchr(s, '?');
    if (!end) end = s + strlen(s);
    const char *fname = s;
    for (const char *p = s; p < end; p++) {
        if (*p == '/') fname = p + 1;
    }
    size_t len = (size_t)(end - fname);

    if (len == strlen("giphy-downsized-medium.gif") &&
        strncmp(fname, "giphy-downsized-medium.gif", len) == 0) {
        return 1;
    }
    if (len == strlen("giphy.gif") &&
        strncmp(fname, "giphy.gif", len) == 0) {
        return 2;
    }
    return 0;
}

/**
 * @brief Parse a single GIF object from the Giphy API response
 *
 * Extracts id, dimensions, timestamps, and fills a giphy_channel_entry_t.
 */
static bool parse_gif_object(const cJSON *gif, giphy_channel_entry_t *out_entry,
                             const char *rendition_name, const char *format_name,
                             bool prefer_downsized,
                             uint16_t screen_width, uint16_t screen_height)
{
    if (!gif || !out_entry) return false;

    // Extract GIF id
    const cJSON *id_json = cJSON_GetObjectItem(gif, "id");
    if (!cJSON_IsString(id_json) || !id_json->valuestring[0]) return false;

    const char *gif_id = id_json->valuestring;
    size_t id_len = strlen(gif_id);
    if (id_len >= sizeof(out_entry->giphy_id)) {
        ESP_LOGW(TAG, "GIF id too long (%zu chars): %.20s...", id_len, gif_id);
        return false;
    }

    memset(out_entry, 0, sizeof(*out_entry));

    // Store giphy_id
    strlcpy(out_entry->giphy_id, gif_id, sizeof(out_entry->giphy_id));

    // Compute post_id
    out_entry->post_id = giphy_id_to_post_id(gif_id);
    out_entry->kind = 0;  // Artwork

    // Determine extension from format
    if (strcmp(format_name, "gif") == 0) {
        out_entry->extension = 1;  // gif
    } else {
        out_entry->extension = 0;  // webp (default)
    }

    // Extract dimensions from the configured rendition
    const cJSON *images = cJSON_GetObjectItem(gif, "images");
    bool used_downsized = false;
    if (cJSON_IsObject(images)) {
        // Check for downsized_medium preference
        if (prefer_downsized) {
            const cJSON *dm = cJSON_GetObjectItem(images, "downsized_medium");
            if (cJSON_IsObject(dm)) {
                uint16_t dm_w = json_dim_u16(cJSON_GetObjectItem(dm, "width"));
                uint16_t dm_h = json_dim_u16(cJSON_GetObjectItem(dm, "height"));
                uint8_t dm_flag = dm_flag_from_url(cJSON_GetObjectItem(dm, "url"));
                if (dm_w > 0 && dm_h > 0 && dm_flag != 0) {
                    // Accept downsized_medium up to 1.5x the screen on each
                    // axis; the renderer downscales modest oversizes and the
                    // result still beats an upscaled fixed_height.
                    uint32_t max_w = (uint32_t)screen_width * 3 / 2;
                    uint32_t max_h = (uint32_t)screen_height * 3 / 2;
                    if (dm_w <= max_w && dm_h <= max_h) {
                        out_entry->width = dm_w;
                        out_entry->height = dm_h;
                        out_entry->extension = 1;  // downsized_medium is always gif
                        out_entry->reserved[0] = dm_flag;  // Which CDN file serves it
                        used_downsized = true;
                        ESP_LOGD(TAG, "Rendition selected: downsized_medium for %s (%ux%u, %s)",
                                 gif_id, dm_w, dm_h,
                                 dm_flag == 2 ? "giphy.gif passthrough" : "dedicated file");
                    } else {
                        ESP_LOGI(TAG, "Rendition rejected: downsized_medium for %s (%ux%u exceeds %ux%u)",
                                 gif_id, dm_w, dm_h, (unsigned)max_w, (unsigned)max_h);
                    }
                }
            }
        }

        if (!used_downsized) {
            const cJSON *rendition = cJSON_GetObjectItem(images, rendition_name);
            if (cJSON_IsObject(rendition)) {
                const cJSON *w = cJSON_GetObjectItem(rendition, "width");
                const cJSON *h = cJSON_GetObjectItem(rendition, "height");
                if (cJSON_IsString(w)) out_entry->width = (uint16_t)atoi(w->valuestring);
                if (cJSON_IsString(h)) out_entry->height = (uint16_t)atoi(h->valuestring);
            }
            if (prefer_downsized) {
                ESP_LOGD(TAG, "Rendition selected: %s for %s (no downsized_medium)",
                         rendition_name, gif_id);
            }
        }
    }

    // Extract timestamp (prefer trending_datetime, fall back to import_datetime)
    const cJSON *trending_dt = cJSON_GetObjectItem(gif, "trending_datetime");
    const cJSON *import_dt = cJSON_GetObjectItem(gif, "import_datetime");
    const char *dt_str = NULL;
    if (cJSON_IsString(trending_dt) && trending_dt->valuestring[0] &&
        strcmp(trending_dt->valuestring, "0000-00-00 00:00:00") != 0) {
        dt_str = trending_dt->valuestring;
    } else if (cJSON_IsString(import_dt) && import_dt->valuestring[0]) {
        dt_str = import_dt->valuestring;
    }

    if (dt_str) {
        // Parse "YYYY-MM-DD HH:MM:SS" format
        struct tm tm = {0};
        if (sscanf(dt_str, "%d-%d-%d %d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            out_entry->created_at = (uint32_t)mktime(&tm);
        }
    }

    if (out_entry->created_at == 0) {
        out_entry->created_at = (uint32_t)time(NULL);
    }

    return true;
}

/**
 * @brief URL-encode a string (RFC 3986)
 *
 * Unreserved characters (A-Z, a-z, 0-9, '-', '_', '.', '~') pass through;
 * everything else is percent-encoded.
 */
static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 3 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

esp_err_t giphy_fetch_random_id(const char *api_key, char *out_random_id, size_t max_len)
{
    if (!api_key || !out_random_id || max_len == 0) return ESP_ERR_INVALID_ARG;
    out_random_id[0] = '\0';

    char url[256];
    snprintf(url, sizeof(url), "https://api.giphy.com/v1/randomid?api_key=%s", api_key);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "random_id: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "random_id: HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char buf[256];
    int total_read = 0;
    int read_len;
    while (total_read < (int)sizeof(buf) - 1) {
        read_len = esp_http_client_read(client, buf + total_read, sizeof(buf) - 1 - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    buf[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "random_id: API returned status %d", status);
        if (status == 429) giphy_set_rate_limited(0);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "random_id: failed to parse JSON");
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *rid = data ? cJSON_GetObjectItem(data, "random_id") : NULL;
    if (cJSON_IsString(rid) && rid->valuestring[0]) {
        strlcpy(out_random_id, rid->valuestring, max_len);
        ESP_LOGI(TAG, "Obtained random_id: %s", out_random_id);
    } else {
        ESP_LOGE(TAG, "random_id: missing or empty in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief Drain an esp_http_client response body into out (null-terminated).
 *
 * Reads up to out_size-1 bytes; further bytes are silently dropped (caller is
 * responsible for sizing). Returns total bytes read.
 */
static int drain_response(esp_http_client_handle_t client, char *out, size_t out_size)
{
    int total = 0;
    while (total < (int)out_size - 1) {
        int n = esp_http_client_read(client, out + total, out_size - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    out[total] = '\0';
    return total;
}

esp_err_t giphy_register_click(const char *api_key,
                               const char *random_id,
                               const char *giphy_id)
{
    if (!api_key || !api_key[0] ||
        !random_id || !random_id[0] ||
        !giphy_id || !giphy_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    // -- Step 1: fetch analytics for this GIF -------------------------------
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.giphy.com/v1/gifs/%s?api_key=%s&fields=id,analytics&random_id=%s",
             giphy_id, api_key, random_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "register_click: HTTP init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_click: HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char body[2048];
    int total = drain_response(client, body, sizeof(body));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGW(TAG, "register_click: lookup HTTP %d for %s", status, giphy_id);
        if (status == 401 || status == 403) return ESP_ERR_NOT_ALLOWED;
        if (status == 429) {
            giphy_set_rate_limited(0);
            return ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_FAIL;
    }
    if (total == 0) {
        ESP_LOGW(TAG, "register_click: empty lookup response");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "register_click: failed to parse lookup JSON");
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *analytics = data ? cJSON_GetObjectItem(data, "analytics") : NULL;
    const cJSON *onclick = analytics ? cJSON_GetObjectItem(analytics, "onclick") : NULL;
    const cJSON *url_node = onclick ? cJSON_GetObjectItem(onclick, "url") : NULL;
    if (!cJSON_IsString(url_node) || !url_node->valuestring[0]) {
        ESP_LOGW(TAG, "register_click: missing analytics.onclick.url for %s", giphy_id);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Compose pingback URL: <onclick_url>&random_id=<rid>&ts=<unix_ms>
    char ping_url[640];
    int64_t now_us = esp_timer_get_time();  // monotonic microseconds since boot
    // Giphy expects unix_ms but tolerates any monotonically increasing value;
    // prefer real wall time when available so analytics line up server-side.
    int64_t unix_ms = (int64_t)time(NULL) * 1000LL + (now_us / 1000LL) % 1000LL;
    int wrote = snprintf(ping_url, sizeof(ping_url),
                         "%s&random_id=%s&ts=%lld",
                         url_node->valuestring, random_id, (long long)unix_ms);
    cJSON_Delete(root);
    if (wrote <= 0 || wrote >= (int)sizeof(ping_url)) {
        ESP_LOGW(TAG, "register_click: pingback URL overflow");
        return ESP_FAIL;
    }

    // -- Step 2: fire the pingback -----------------------------------------
    esp_http_client_config_t pcfg = {
        .url = ping_url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t pclient = esp_http_client_init(&pcfg);
    if (!pclient) {
        ESP_LOGE(TAG, "register_click: pingback HTTP init failed");
        return ESP_FAIL;
    }

    err = esp_http_client_open(pclient, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_click: pingback HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(pclient);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(pclient);
    int pstatus = esp_http_client_get_status_code(pclient);
    char pbody[64];
    drain_response(pclient, pbody, sizeof(pbody));
    esp_http_client_close(pclient);
    esp_http_client_cleanup(pclient);

    if (pstatus != 200) {
        ESP_LOGW(TAG, "register_click: pingback HTTP %d for %s", pstatus, giphy_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "register_click: ok for %s", giphy_id);
    return ESP_OK;
}

// Giphy beta keys get a fixed local cooldown (see giphy_set_rate_limited); we
// intentionally do not honor the server's Retry-After, so the arg is ignored.
static void giphy_page_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    (void)retry_after_sec;
    (void)ctx;
    giphy_set_rate_limited(0);
}

esp_err_t giphy_fetch_page(giphy_fetch_ctx_t *ctx, int offset,
                           giphy_channel_entry_t *out_entries,
                           size_t *out_count, bool *out_has_more)
{
    if (!ctx || !out_entries || !out_count || !out_has_more) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    *out_has_more = false;

    bool is_search = (ctx->query[0] != '\0');

    // When prefer_downsized is active and rendition is fixed_height,
    // request both renditions so we can pick the best per entry.
    bool request_downsized = ctx->prefer_downsized &&
                             strcmp(ctx->rendition, "fixed_height") == 0;

    // Build URL
    char url[512];
    if (is_search) {
        char encoded_query[192];
        url_encode(ctx->query, encoded_query, sizeof(encoded_query));
        if (request_downsized) {
            snprintf(url, sizeof(url),
                     "https://api.giphy.com/v1/gifs/search?api_key=%s&q=%s&limit=%d&offset=%d&rating=%s"
                     "&fields=id,trending_datetime,import_datetime,images.%s,images.downsized_medium",
                     ctx->api_key, encoded_query, GIPHY_PAGE_LIMIT, offset, ctx->rating, ctx->rendition);
        } else {
            snprintf(url, sizeof(url),
                     "https://api.giphy.com/v1/gifs/search?api_key=%s&q=%s&limit=%d&offset=%d&rating=%s"
                     "&fields=id,trending_datetime,import_datetime,images.%s",
                     ctx->api_key, encoded_query, GIPHY_PAGE_LIMIT, offset, ctx->rating, ctx->rendition);
        }
        ESP_LOGI(TAG, "Fetching search q=\"%s\": offset=%d, limit=%d", ctx->query, offset, GIPHY_PAGE_LIMIT);
    } else {
        if (request_downsized) {
            snprintf(url, sizeof(url),
                     "https://api.giphy.com/v1/gifs/trending?api_key=%s&limit=%d&offset=%d&rating=%s"
                     "&fields=id,trending_datetime,import_datetime,images.%s,images.downsized_medium",
                     ctx->api_key, GIPHY_PAGE_LIMIT, offset, ctx->rating, ctx->rendition);
        } else {
            snprintf(url, sizeof(url),
                     "https://api.giphy.com/v1/gifs/trending?api_key=%s&limit=%d&offset=%d&rating=%s"
                     "&fields=id,trending_datetime,import_datetime,images.%s",
                     ctx->api_key, GIPHY_PAGE_LIMIT, offset, ctx->rating, ctx->rendition);
        }
        ESP_LOGI(TAG, "Fetching trending: offset=%d, limit=%d", offset, GIPHY_PAGE_LIMIT);
    }

    // Append random_id if available
    if (ctx->random_id[0] != '\0') {
        size_t url_len = strlen(url);
        snprintf(url + url_len, sizeof(url) - url_len, "&random_id=%s", ctx->random_id);
    }

    // Append country_code if set
    if (ctx->country_code[0] != '\0') {
        size_t url_len = strlen(url);
        snprintf(url + url_len, sizeof(url) - url_len, "&country_code=%s", ctx->country_code);
    }

    http_fetch_request_t fr = {
        .url = url,
        .on_rate_limited = giphy_page_on_rate_limited,
    };
    size_t got = 0;
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, ctx->response_buf, ctx->response_buf_size,
                                         &got, &res);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Giphy page fetch failed: %s (HTTP %d)",
                 esp_err_to_name(err), res.http_status);
        return err;
    }
    int total_read = (int)got;

    ESP_LOGD(TAG, "Received %d bytes from Giphy API", total_read);

    if (total_read > 0 && total_read < 150) {
        ESP_LOGW(TAG, "Small response (%d bytes), full body: %s", total_read, ctx->response_buf);
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(ctx->response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse Giphy JSON response (%d bytes)", total_read);
        char snippet[201];
        int snip_len = total_read < 200 ? total_read : 200;
        memcpy(snippet, ctx->response_buf, snip_len);
        snippet[snip_len] = '\0';
        ESP_LOGE(TAG, "Response start: %.200s", snippet);
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "Giphy response missing 'data' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int array_size = cJSON_GetArraySize(data);
    if (array_size == 0) {
        ESP_LOGI(TAG, "No more results at offset %d", offset);
        cJSON_Delete(root);
        return ESP_OK;
    }

    // Parse each GIF object. out_entries holds exactly GIPHY_PAGE_LIMIT slots
    // (allocated by the caller in giphy_refresh.c; contract in giphy.h), so a
    // server that ignores our limit=50 and returns more items must never write
    // past the buffer.
    if (array_size > GIPHY_PAGE_LIMIT) {
        ESP_LOGW(TAG, "Giphy returned %d items (> limit %d); truncating",
                 array_size, GIPHY_PAGE_LIMIT);
    }
    size_t parsed = 0;
    for (int i = 0; i < array_size && parsed < GIPHY_PAGE_LIMIT; i++) {
        const cJSON *gif = cJSON_GetArrayItem(data, i);
        if (parse_gif_object(gif, &out_entries[parsed], ctx->rendition, ctx->format,
                             request_downsized, ctx->screen_width, ctx->screen_height)) {
            parsed++;
        }
    }

    ESP_LOGI(TAG, "Parsed %zu/%d GIFs at offset %d", parsed, array_size, offset);

    cJSON_Delete(root);

    *out_count = parsed;
    // Cap at 499 for both trending and search to prevent API key exhaustion.
    // The search endpoint supports up to 4999, but we intentionally use the
    // same conservative limit as trending.
    *out_has_more = (array_size >= GIPHY_PAGE_LIMIT && (offset + array_size) < 499);

    return ESP_OK;
}
