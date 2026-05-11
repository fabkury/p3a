// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file config_store_art_institution.c
 * @brief Config store accessors for the two global museum settings:
 *        ai_refresh_sec (refresh interval) and ai_cache_size (per-channel
 *        max entries).
 *
 * Mirrors the cached-with-invalidate pattern used by config_store_giphy.c
 * (giphy_cache_size / giphy_refresh_interval), so other components (the
 * refresh dispatcher and the merge step in art_institution_refresh.c) can
 * call the getters on every dispatcher tick without burning NVS reads.
 */

#include "config_store_internal.h"

static const char *TAG = "CFG";

// ============================================================================
// ai_refresh_sec — museum-channel refresh interval (seconds)
// ============================================================================

#define AI_REFRESH_SEC_DEFAULT 86400u   // 1 day (design §8)
#define AI_REFRESH_SEC_MIN     28800u   // 8 h
#define AI_REFRESH_SEC_MAX     345600u  // 4 d

static uint32_t s_ai_refresh_sec = AI_REFRESH_SEC_DEFAULT;
static bool s_ai_refresh_sec_loaded = false;

esp_err_t config_store_set_ai_refresh_sec(uint32_t seconds)
{
    if (seconds < AI_REFRESH_SEC_MIN || seconds > AI_REFRESH_SEC_MAX) {
        ESP_LOGE(TAG, "Invalid ai_refresh_sec: %lu (must be %u-%u)",
                 (unsigned long)seconds, AI_REFRESH_SEC_MIN, AI_REFRESH_SEC_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "ai_refresh_sec");
    if (item) {
        cJSON_SetNumberValue(item, (double)seconds);
    } else {
        cJSON_AddNumberToObject(cfg, "ai_refresh_sec", (double)seconds);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_ai_refresh_sec = seconds;
        s_ai_refresh_sec_loaded = true;
        ESP_LOGI(TAG, "ai_refresh_sec saved: %lu", (unsigned long)seconds);
    }

    return err;
}

uint32_t config_store_get_ai_refresh_sec(void)
{
    if (s_ai_refresh_sec_loaded) {
        return s_ai_refresh_sec;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_ai_refresh_sec_loaded = true;
        return s_ai_refresh_sec;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "ai_refresh_sec");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= AI_REFRESH_SEC_MIN && value <= AI_REFRESH_SEC_MAX) {
            s_ai_refresh_sec = (uint32_t)value;
        }
    }

    s_ai_refresh_sec_loaded = true;
    cJSON_Delete(cfg);
    return s_ai_refresh_sec;
}

void config_store_invalidate_ai_refresh_sec(void)
{
    s_ai_refresh_sec_loaded = false;
}

// ============================================================================
// ai_cache_size — museum-channel per-channel max cache entries
// ============================================================================

#define AI_CACHE_SIZE_DEFAULT 1024u  // design §8
#define AI_CACHE_SIZE_MIN     32u
#define AI_CACHE_SIZE_MAX     4096u  // matches CHANNEL_CACHE_HARD_CAP

static uint32_t s_ai_cache_size = AI_CACHE_SIZE_DEFAULT;
static bool s_ai_cache_size_loaded = false;

esp_err_t config_store_set_ai_cache_size(uint32_t size)
{
    if (size < AI_CACHE_SIZE_MIN || size > AI_CACHE_SIZE_MAX) {
        ESP_LOGE(TAG, "Invalid ai_cache_size: %lu (must be %u-%u)",
                 (unsigned long)size, AI_CACHE_SIZE_MIN, AI_CACHE_SIZE_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "ai_cache_size");
    if (item) {
        cJSON_SetNumberValue(item, (double)size);
    } else {
        cJSON_AddNumberToObject(cfg, "ai_cache_size", (double)size);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_ai_cache_size = size;
        s_ai_cache_size_loaded = true;
        ESP_LOGI(TAG, "ai_cache_size saved: %lu", (unsigned long)size);
    }

    return err;
}

uint32_t config_store_get_ai_cache_size(void)
{
    if (s_ai_cache_size_loaded) {
        return s_ai_cache_size;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_ai_cache_size_loaded = true;
        return s_ai_cache_size;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "ai_cache_size");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= AI_CACHE_SIZE_MIN && value <= AI_CACHE_SIZE_MAX) {
            s_ai_cache_size = (uint32_t)value;
        }
    }

    s_ai_cache_size_loaded = true;
    cJSON_Delete(cfg);
    return s_ai_cache_size;
}

void config_store_invalidate_ai_cache_size(void)
{
    s_ai_cache_size_loaded = false;
}
