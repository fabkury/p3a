// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file config_store_klipy.c
 * @brief Config store Klipy-specific settings: API key, format, rating,
 *        cache size, refresh interval. Mirrors config_store_giphy.c.
 */

#include "config_store_internal.h"

static const char *TAG = "CFG";

// ============================================================================
// Klipy Settings (persisted)
// ============================================================================

esp_err_t config_store_set_klipy_api_key(const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    if (key[0] == '\0') {
        // Empty key: remove the entry so the getter falls back to the (empty)
        // build-time default.
        cJSON *cfg = NULL;
        esp_err_t err = config_store_load(&cfg);
        if (err != ESP_OK) return err;
        cJSON_DeleteItemFromObject(cfg, "klipy_api_key");
        err = config_store_save(cfg);
        cJSON_Delete(cfg);
        return err;
    }
    return cfg_set_string("klipy_api_key", key);
}

esp_err_t config_store_get_klipy_api_key(char *out_key, size_t max_len)
{
    if (!out_key || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("klipy_api_key", CONFIG_KLIPY_API_KEY_DEFAULT, out_key, max_len);
}

esp_err_t config_store_set_klipy_format(const char *format)
{
    if (!format || format[0] == '\0') return ESP_ERR_INVALID_ARG;
    return cfg_set_string("klipy_format", format);
}

esp_err_t config_store_get_klipy_format(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("klipy_format", CONFIG_KLIPY_FORMAT_DEFAULT, out, max_len);
}

esp_err_t config_store_set_klipy_rating(const char *rating)
{
    if (!rating || rating[0] == '\0') return ESP_ERR_INVALID_ARG;
    return cfg_set_string("klipy_rating", rating);
}

esp_err_t config_store_get_klipy_rating(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("klipy_rating", CONFIG_KLIPY_RATING_DEFAULT, out, max_len);
}

// ============================================================================
// Klipy Cache Size (persisted, cached)
// ============================================================================
// Max is far above Giphy's 500: Klipy trending reaches at least ~2500 items,
// so a channel can hold a much larger, more varied pool.

#define KLIPY_CACHE_SIZE_DEFAULT 350
#define KLIPY_CACHE_SIZE_MIN     32
#define KLIPY_CACHE_SIZE_MAX     2500

static uint32_t s_klipy_cache_size = KLIPY_CACHE_SIZE_DEFAULT;
static bool s_klipy_cache_size_loaded = false;

esp_err_t config_store_set_klipy_cache_size(uint32_t size)
{
    if (size < KLIPY_CACHE_SIZE_MIN || size > KLIPY_CACHE_SIZE_MAX) {
        ESP_LOGE(TAG, "Invalid klipy cache size: %lu (must be %d-%d)",
                 (unsigned long)size, KLIPY_CACHE_SIZE_MIN, KLIPY_CACHE_SIZE_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "klipy_cache_size");
    if (item) {
        cJSON_SetNumberValue(item, (double)size);
    } else {
        cJSON_AddNumberToObject(cfg, "klipy_cache_size", (double)size);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_klipy_cache_size = size;
        s_klipy_cache_size_loaded = true;
        ESP_LOGI(TAG, "Klipy cache size saved: %lu", (unsigned long)size);
    }
    return err;
}

uint32_t config_store_get_klipy_cache_size(void)
{
    if (s_klipy_cache_size_loaded) {
        return s_klipy_cache_size;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_klipy_cache_size_loaded = true;
        return s_klipy_cache_size;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "klipy_cache_size");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= KLIPY_CACHE_SIZE_MIN && value <= KLIPY_CACHE_SIZE_MAX) {
            s_klipy_cache_size = (uint32_t)value;
        }
    }

    s_klipy_cache_size_loaded = true;
    cJSON_Delete(cfg);
    return s_klipy_cache_size;
}

void config_store_invalidate_klipy_cache_size(void)
{
    s_klipy_cache_size_loaded = false;
}

// ============================================================================
// Klipy Refresh Interval (persisted, cached)
// ============================================================================

#define KLIPY_REFRESH_INTERVAL_DEFAULT 7200

static uint32_t s_klipy_refresh_interval = KLIPY_REFRESH_INTERVAL_DEFAULT;
static bool s_klipy_refresh_interval_loaded = false;

esp_err_t config_store_set_klipy_refresh_interval(uint32_t seconds)
{
    if (seconds < 60 || seconds > 28800) {
        ESP_LOGE(TAG, "Invalid klipy refresh interval: %lu sec", (unsigned long)seconds);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "klipy_refresh_interval");
    if (item) {
        cJSON_SetNumberValue(item, (double)seconds);
    } else {
        cJSON_AddNumberToObject(cfg, "klipy_refresh_interval", (double)seconds);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_klipy_refresh_interval = seconds;
        s_klipy_refresh_interval_loaded = true;
        ESP_LOGI(TAG, "Klipy refresh interval saved: %lu seconds", (unsigned long)seconds);
    }
    return err;
}

uint32_t config_store_get_klipy_refresh_interval(void)
{
    if (s_klipy_refresh_interval_loaded) {
        return s_klipy_refresh_interval;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_klipy_refresh_interval_loaded = true;
        return s_klipy_refresh_interval;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "klipy_refresh_interval");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= 60 && value <= 28800) {
            s_klipy_refresh_interval = (uint32_t)value;
        }
    }

    s_klipy_refresh_interval_loaded = true;
    cJSON_Delete(cfg);
    return s_klipy_refresh_interval;
}

void config_store_invalidate_klipy_refresh_interval(void)
{
    s_klipy_refresh_interval_loaded = false;
}
