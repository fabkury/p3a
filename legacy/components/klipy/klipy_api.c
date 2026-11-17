// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy_api.c
 * @brief Klipy API client - fetches trending and search items via HTTP
 *
 * The rate-limit cooldown and auth-invalid / no-key latches are copied verbatim
 * from giphy_api.c (same semantics). The one Klipy-specific difference is auth
 * detection: Klipy rejects a bad API key with HTTP 404 on an otherwise-valid
 * route (never 401/403), so klipy_fetch_page latches on ESP_ERR_NOT_FOUND.
 */

#include "klipy.h"
#include "klipy_types.h"
#include "klipy_internal.h"
#include "http_fetch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "klipy_api";

// ============================================================================
// Process-wide rate-limit cooldown (see giphy_api.c for rationale)
// ============================================================================
#define KLIPY_DEFAULT_COOLDOWN_SEC 600

static int64_t s_cooldown_until_us = 0;

void klipy_set_rate_limited(uint32_t cooldown_sec)
{
    if (cooldown_sec == 0) cooldown_sec = KLIPY_DEFAULT_COOLDOWN_SEC;
    int64_t until = esp_timer_get_time() + (int64_t)cooldown_sec * 1000000LL;
    if (until > s_cooldown_until_us) {
        s_cooldown_until_us = until;
        ESP_LOGW(TAG, "Klipy rate-limit cooldown engaged: %lus", (unsigned long)cooldown_sec);
    }
}

bool klipy_is_rate_limited(void)
{
    return esp_timer_get_time() < s_cooldown_until_us;
}

uint32_t klipy_cooldown_remaining_sec(void)
{
    int64_t now = esp_timer_get_time();
    if (now >= s_cooldown_until_us) return 0;
    return (uint32_t)((s_cooldown_until_us - now + 999999) / 1000000);
}

// ============================================================================
// Process-wide auth-invalid latch + no-key parking (see giphy_api.c)
// ============================================================================
#define KLIPY_AUTH_REPROBE_SEC 3600

static int64_t s_auth_invalid_until_us = 0;
static uint32_t s_auth_bad_key_fnv = 0;
static bool s_auth_notified = false;

static uint32_t auth_key_fnv(const char *key)
{
    uint32_t h = 0x811c9dc5u;
    if (!key) return h;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

void klipy_set_auth_invalid(const char *api_key)
{
    int64_t until = esp_timer_get_time() + (int64_t)KLIPY_AUTH_REPROBE_SEC * 1000000LL;
    if (until > s_auth_invalid_until_us) s_auth_invalid_until_us = until;
    s_auth_bad_key_fnv = auth_key_fnv(api_key);
    ESP_LOGW(TAG, "Klipy rejected the API key (HTTP 404 on a valid route); suspending "
                  "Klipy refreshes for %us. Saving a key in Settings retries immediately.",
             (unsigned)KLIPY_AUTH_REPROBE_SEC);
}

bool klipy_is_auth_invalid(void)
{
    return esp_timer_get_time() < s_auth_invalid_until_us;
}

uint32_t klipy_auth_invalid_remaining_sec(void)
{
    int64_t now = esp_timer_get_time();
    if (now >= s_auth_invalid_until_us) return 0;
    return (uint32_t)((s_auth_invalid_until_us - now + 999999) / 1000000);
}

static int64_t s_no_key_until_us = 0;
static bool s_no_key_notified = false;

void klipy_set_no_key(void)
{
    int64_t until = esp_timer_get_time() + (int64_t)KLIPY_AUTH_REPROBE_SEC * 1000000LL;
    if (until > s_no_key_until_us) s_no_key_until_us = until;
}

bool klipy_is_no_key(void)
{
    return esp_timer_get_time() < s_no_key_until_us;
}

uint32_t klipy_no_key_remaining_sec(void)
{
    int64_t now = esp_timer_get_time();
    if (now >= s_no_key_until_us) return 0;
    return (uint32_t)((s_no_key_until_us - now + 999999) / 1000000);
}

bool klipy_no_key_take_notification(void)
{
    if (!klipy_is_no_key() || s_no_key_notified) return false;
    s_no_key_notified = true;
    return true;
}

void klipy_clear_auth_invalid(void)
{
    if (s_auth_invalid_until_us == 0 && s_auth_bad_key_fnv == 0 && !s_auth_notified &&
        s_no_key_until_us == 0 && !s_no_key_notified) {
        return;
    }
    s_auth_invalid_until_us = 0;
    s_auth_bad_key_fnv = 0;
    s_auth_notified = false;
    s_no_key_until_us = 0;
    s_no_key_notified = false;
    ESP_LOGI(TAG, "Klipy key-parking state cleared (auth-invalid latch / no-key flag)");
}

bool klipy_auth_invalid_for_key(const char *api_key)
{
    if (!klipy_is_auth_invalid()) return false;
    if (auth_key_fnv(api_key) != s_auth_bad_key_fnv) {
        klipy_clear_auth_invalid();
        return false;
    }
    return true;
}

bool klipy_auth_take_notification(void)
{
    if (!klipy_is_auth_invalid() || s_auth_notified) return false;
    s_auth_notified = true;
    return true;
}

// ============================================================================
// Rendition selection (shared with klipy_download.c)
// ============================================================================

static uint16_t json_dim_u16(const cJSON *v)
{
    if (cJSON_IsNumber(v) && v->valuedouble > 0 && v->valuedouble <= UINT16_MAX) {
        return (uint16_t)v->valuedouble;
    }
    if (cJSON_IsString(v) && v->valuestring[0]) {
        int n = atoi(v->valuestring);
        return (n > 0 && n <= UINT16_MAX) ? (uint16_t)n : 0;
    }
    return 0;
}

bool klipy_pick_rendition(const cJSON *file, const char *fmt_pref,
                          uint16_t screen_w, uint16_t screen_h,
                          const char **out_url, uint16_t *out_w, uint16_t *out_h,
                          bool *out_used_gif)
{
    if (!cJSON_IsObject(file) || !fmt_pref) return false;

    static const char *tiers[] = { "hd", "md", "sm", "xs" };
    const char *formats[2];
    formats[0] = fmt_pref;
    formats[1] = (strcmp(fmt_pref, "gif") == 0) ? "webp" : "gif";

    uint32_t max_w = (uint32_t)screen_w * 3 / 2;
    uint32_t max_h = (uint32_t)screen_h * 3 / 2;

    for (int f = 0; f < 2; f++) {
        const char *best_url = NULL, *fit_url = NULL;
        uint16_t bw = 0, bh = 0, fw = 0, fh = 0;
        uint32_t best_area = 0, fit_area = 0;
        bool have_best = false, have_fit = false;

        for (int t = 0; t < 4; t++) {
            const cJSON *tn = cJSON_GetObjectItem(file, tiers[t]);
            if (!cJSON_IsObject(tn)) continue;
            const cJSON *fn = cJSON_GetObjectItem(tn, formats[f]);
            if (!cJSON_IsObject(fn)) continue;
            const cJSON *url = cJSON_GetObjectItem(fn, "url");
            if (!cJSON_IsString(url) || !url->valuestring[0]) continue;

            uint16_t w = json_dim_u16(cJSON_GetObjectItem(fn, "width"));
            uint16_t h = json_dim_u16(cJSON_GetObjectItem(fn, "height"));
            uint32_t area = (uint32_t)w * (uint32_t)h;

            if (!have_best || area >= best_area) {
                have_best = true; best_area = area; bw = w; bh = h; best_url = url->valuestring;
            }
            if (w <= max_w && h <= max_h && (!have_fit || area >= fit_area)) {
                have_fit = true; fit_area = area; fw = w; fh = h; fit_url = url->valuestring;
            }
        }

        if (have_fit) {
            *out_url = fit_url; *out_w = fw; *out_h = fh;
            *out_used_gif = (strcmp(formats[f], "gif") == 0);
            return true;
        }
        if (have_best) {
            *out_url = best_url; *out_w = bw; *out_h = bh;
            *out_used_gif = (strcmp(formats[f], "gif") == 0);
            return true;
        }
    }
    return false;
}

// ============================================================================
// Item parsing
// ============================================================================

static bool parse_klipy_item(const cJSON *it, klipy_channel_entry_t *out_entry,
                             uint8_t product_id, const char *format_name,
                             uint16_t screen_width, uint16_t screen_height)
{
    if (!cJSON_IsObject(it) || !out_entry) return false;

    // Skip sponsored items — only present if ad params are requested (they are
    // not here), but be defensive.
    const cJSON *type = cJSON_GetObjectItem(it, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "ad") == 0) return false;

    const cJSON *id = cJSON_GetObjectItem(it, "id");
    uint64_t kid = 0;
    if (cJSON_IsNumber(id)) {
        kid = (uint64_t)id->valuedouble;   // 16-digit ids fit exactly in a double (< 2^53)
    } else if (cJSON_IsString(id) && id->valuestring[0]) {
        kid = strtoull(id->valuestring, NULL, 10);
    }
    if (kid == 0) return false;

    const cJSON *file = cJSON_GetObjectItem(it, "file");
    const char *url = NULL;
    uint16_t w = 0, h = 0;
    bool used_gif = false;
    if (!klipy_pick_rendition(file, format_name, screen_width, screen_height,
                              &url, &w, &h, &used_gif)) {
        return false;
    }

    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->klipy_id = kid;
    out_entry->post_id = klipy_id_to_post_id(kid);
    out_entry->kind = 0;
    out_entry->product = product_id;
    out_entry->extension = used_gif ? 1 : 0;
    out_entry->width = w;
    out_entry->height = h;
    out_entry->created_at = (uint32_t)time(NULL);  // Klipy items carry no timestamp
    return true;
}

// ============================================================================
// URL helpers
// ============================================================================

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

static void klipy_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    (void)retry_after_sec;
    (void)ctx;
    klipy_set_rate_limited(0);
}

// ============================================================================
// Page fetch
// ============================================================================

esp_err_t klipy_fetch_page(klipy_fetch_ctx_t *ctx, int page,
                           klipy_channel_entry_t *out_entries,
                           size_t *out_count, bool *out_has_more)
{
    if (!ctx || !out_entries || !out_count || !out_has_more) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    *out_has_more = false;

    const char *product = ctx->product[0] ? ctx->product : "gifs";
    bool is_search = (ctx->query[0] != '\0');

    char url[640];
    if (is_search) {
        char encoded_query[192];
        url_encode(ctx->query, encoded_query, sizeof(encoded_query));
        snprintf(url, sizeof(url),
                 "https://api.klipy.com/api/v1/%s/%s/search?q=%s&page=%d&per_page=%d&rating=%s",
                 ctx->api_key, product, encoded_query, page, KLIPY_PAGE_LIMIT, ctx->rating);
        ESP_LOGI(TAG, "Fetching %s/search q=\"%s\": page=%d", product, ctx->query, page);
    } else {
        snprintf(url, sizeof(url),
                 "https://api.klipy.com/api/v1/%s/%s/trending?page=%d&per_page=%d&rating=%s",
                 ctx->api_key, product, page, KLIPY_PAGE_LIMIT, ctx->rating);
        ESP_LOGI(TAG, "Fetching %s/trending: page=%d", product, page);
    }

    http_fetch_request_t fr = {
        .url = url,
        .on_rate_limited = klipy_on_rate_limited,
    };
    size_t got = 0;
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, ctx->response_buf, ctx->response_buf_size,
                                         &got, &res);
    if (err != ESP_OK) {
        // Do NOT log the response body — Klipy echoes the API key in error
        // messages. Log status only.
        ESP_LOGE(TAG, "Klipy page fetch failed: %s (HTTP %d)",
                 esp_err_to_name(err), res.http_status);
        // A rejected key is HTTP 404 on a valid route (Klipy never emits
        // 401/403). trending/search always exist for a good key, so a 404 here
        // means the key path segment was rejected -> latch auth-invalid.
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_ALLOWED) {
            klipy_set_auth_invalid(ctx->api_key);
            return ESP_ERR_NOT_ALLOWED;
        }
        return err;  // 429 -> ESP_ERR_INVALID_RESPONSE (cooldown engaged via cb)
    }

    cJSON *root = cJSON_Parse(ctx->response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse Klipy JSON response (%d bytes)", (int)got);
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *arr = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "data") : NULL;
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "Klipy response missing data.data array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *has_next = cJSON_GetObjectItem(data, "has_next");
    int array_size = cJSON_GetArraySize(arr);

    size_t parsed = 0;
    for (int i = 0; i < array_size && parsed < KLIPY_PAGE_LIMIT; i++) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        if (parse_klipy_item(it, &out_entries[parsed], ctx->product_id, ctx->format,
                             ctx->screen_width, ctx->screen_height)) {
            parsed++;
        }
    }

    ESP_LOGI(TAG, "Parsed %zu/%d items at page %d", parsed, array_size, page);

    cJSON_Delete(root);

    *out_count = parsed;
    *out_has_more = cJSON_IsBool(has_next) ? cJSON_IsTrue(has_next)
                                           : (array_size >= KLIPY_PAGE_LIMIT);
    return ESP_OK;
}
