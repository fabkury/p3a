// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "config_store_internal.h"

static const char *TAG = "CFG";

// ============================================================================
// Internal helpers
// ============================================================================

uint8_t cfg_clamp_u8_num(const cJSON *n, uint8_t def)
{
    if (!n || !cJSON_IsNumber((cJSON *)n)) {
        return def;
    }
    double v = cJSON_GetNumberValue((cJSON *)n);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

esp_err_t cfg_ensure_nvs(nvs_handle_t *h) {
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, h);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        // NVS already initialized in app_main, but safe to call again
        nvs_flash_init();
        err = nvs_open(NAMESPACE, NVS_READWRITE, h);
    }
    return err;
}

// Helper: set a string config field
esp_err_t cfg_set_string(const char *key, const char *value)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, key);
    if (item) {
        cJSON_DeleteItemFromObject(cfg, key);
    }
    cJSON_AddStringToObject(cfg, key, value);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    return err;
}

// Helper: get a string config field
esp_err_t cfg_get_string(const char *key, const char *default_val,
                                char *out, size_t max_len)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        strlcpy(out, default_val, max_len);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, key);
    if (item && cJSON_IsString(item) && item->valuestring[0]) {
        strlcpy(out, item->valuestring, max_len);
        cJSON_Delete(cfg);
        return ESP_OK;
    }

    cJSON_Delete(cfg);
    strlcpy(out, default_val, max_len);
    return ESP_ERR_NOT_FOUND;
}

// ============================================================================
// Core API
// ============================================================================

esp_err_t config_store_get_serialized(char **out_json, size_t *out_len) {
    nvs_handle_t h;
    esp_err_t err = cfg_ensure_nvs(&h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    size_t sz = 0;
    err = nvs_get_blob(h, KEY_CUR, NULL, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Default empty object
        const char *empty = "{}";
        *out_len = strlen(empty);
        *out_json = malloc(*out_len + 1);
        if (!*out_json) {
            nvs_close(h);
            return ESP_ERR_NO_MEM;
        }
        memcpy(*out_json, empty, *out_len + 1);
        nvs_close(h);
        return ESP_OK;
    }

    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "Failed to get blob size: %s", esp_err_to_name(err));
        return err;
    }

    if (sz > MAX_JSON) {
        nvs_close(h);
        ESP_LOGE(TAG, "Config blob too large: %zu bytes", sz);
        return ESP_ERR_NO_MEM;
    }

    *out_json = malloc(sz + 1);
    if (!*out_json) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(h, KEY_CUR, *out_json, &sz);
    nvs_close(h);
    if (err != ESP_OK) {
        free(*out_json);
        ESP_LOGE(TAG, "Failed to read blob: %s", esp_err_to_name(err));
        return err;
    }

    (*out_json)[sz] = '\0';
    *out_len = sz;
    return ESP_OK;
}

esp_err_t config_store_load(cJSON **out_cfg) {
    char *json;
    size_t len;
    esp_err_t err = config_store_get_serialized(&json, &len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *o = cJSON_ParseWithLength(json, len);
    free(json);

    if (!o) {
        ESP_LOGW(TAG, "Failed to parse config JSON, using empty object");
        // Return empty object instead of failing
        o = cJSON_CreateObject();
        if (!o) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        ESP_LOGE(TAG, "Config is not a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    // Keep runtime caches in sync (cheap; uses parsed JSON we already have).
    cfg_bg_apply_from_cfg(o);
    cfg_show_fps_apply_from_cfg(o);
    cfg_max_speed_playback_apply_from_cfg(o);

    *out_cfg = o;
    return ESP_OK;
}

esp_err_t config_store_save(const cJSON *cfg) {
    if (!cfg || !cJSON_IsObject((cJSON*)cfg)) {
        ESP_LOGE(TAG, "Invalid config: must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    // Serialize compact JSON
    char *serialized = cJSON_PrintBuffered((cJSON*)cfg, 1024, false);
    if (!serialized) {
        ESP_LOGE(TAG, "Failed to serialize config");
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(serialized);
    if (len > MAX_JSON) {
        free(serialized);
        ESP_LOGE(TAG, "Serialized config too large: %zu bytes (max %d)", len, MAX_JSON);
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    esp_err_t err = cfg_ensure_nvs(&h);
    if (err != ESP_OK) {
        free(serialized);
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Atomic save: write to temp key first
    err = nvs_set_blob(h, KEY_NEW, serialized, len);
    if (err != ESP_OK) {
        nvs_close(h);
        free(serialized);
        ESP_LOGE(TAG, "Failed to write temp blob: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        nvs_close(h);
        free(serialized);
        ESP_LOGE(TAG, "Failed to commit temp blob: %s", esp_err_to_name(err));
        return err;
    }

    // Validate readback
    size_t verify_sz = 0;
    err = nvs_get_blob(h, KEY_NEW, NULL, &verify_sz);
    if (err != ESP_OK || verify_sz != len) {
        nvs_close(h);
        free(serialized);
        ESP_LOGE(TAG, "Failed to verify temp blob");
        return ESP_FAIL;
    }

    // Swap: write to main key
    err = nvs_set_blob(h, KEY_CUR, serialized, len);
    if (err != ESP_OK) {
        nvs_close(h);
        free(serialized);
        ESP_LOGE(TAG, "Failed to write main blob: %s", esp_err_to_name(err));
        return err;
    }

    // Erase temp key
    nvs_erase_key(h, KEY_NEW);

    err = nvs_commit(h);
    nvs_close(h);
    free(serialized);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit main blob: %s", esp_err_to_name(err));
        return err;
    }

    // Update runtime caches from the config we just saved.
    cfg_bg_apply_from_cfg(cfg);
    cfg_show_fps_apply_from_cfg(cfg);
    cfg_max_speed_playback_apply_from_cfg(cfg);

    ESP_LOGI(TAG, "Config saved successfully (%zu bytes)", len);
    return ESP_OK;
}
