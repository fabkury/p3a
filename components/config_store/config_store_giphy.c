// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "config_store_internal.h"

static const char *TAG = "CFG";

// ============================================================================
// Giphy Settings (persisted)
// ============================================================================

esp_err_t config_store_set_giphy_api_key(const char *key)
{
    if (!key || key[0] == '\0') return ESP_ERR_INVALID_ARG;
    return cfg_set_string("giphy_api_key", key);
}

esp_err_t config_store_get_giphy_api_key(char *out_key, size_t max_len)
{
    if (!out_key || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("giphy_api_key", CONFIG_GIPHY_API_KEY_DEFAULT, out_key, max_len);
}

esp_err_t config_store_set_giphy_rendition(const char *rendition)
{
    if (!rendition || rendition[0] == '\0') return ESP_ERR_INVALID_ARG;
    return cfg_set_string("giphy_rendition", rendition);
}

esp_err_t config_store_get_giphy_rendition(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("giphy_rendition", CONFIG_GIPHY_RENDITION_DEFAULT, out, max_len);
}

esp_err_t config_store_set_giphy_format(const char *format)
{
    if (!format || format[0] == '\0') return ESP_ERR_INVALID_ARG;
    return cfg_set_string("giphy_format", format);
}

esp_err_t config_store_get_giphy_format(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("giphy_format", CONFIG_GIPHY_FORMAT_DEFAULT, out, max_len);
}

esp_err_t config_store_set_giphy_rating(const char *rating)
{
    if (!rating || rating[0] == '\0') return ESP_ERR_INVALID_ARG;
    return cfg_set_string("giphy_rating", rating);
}

esp_err_t config_store_get_giphy_rating(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("giphy_rating", "pg-13", out, max_len);
}

esp_err_t config_store_set_giphy_country_code(const char *code)
{
    if (!code) return ESP_ERR_INVALID_ARG;
    if (code[0] == '\0') {
        cJSON *cfg = NULL;
        esp_err_t err = config_store_load(&cfg);
        if (err != ESP_OK) return err;
        cJSON_DeleteItemFromObject(cfg, "giphy_country_code");
        err = config_store_save(cfg);
        cJSON_Delete(cfg);
        return err;
    }
    return cfg_set_string("giphy_country_code", code);
}

esp_err_t config_store_get_giphy_country_code(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    return cfg_get_string("giphy_country_code", "", out, max_len);
}

// ============================================================================
// Giphy Random ID (persisted, cached)
// ============================================================================

static char s_giphy_random_id[40] = "";
static bool s_giphy_random_id_loaded = false;

esp_err_t config_store_set_giphy_random_id(const char *random_id)
{
    if (!random_id) return ESP_ERR_INVALID_ARG;

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON_DeleteItemFromObject(cfg, "giphy_random_id");
    cJSON_AddStringToObject(cfg, "giphy_random_id", random_id);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    if (err == ESP_OK) {
        strlcpy(s_giphy_random_id, random_id, sizeof(s_giphy_random_id));
        s_giphy_random_id_loaded = true;
    }
    return err;
}

esp_err_t config_store_get_giphy_random_id(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;

    if (s_giphy_random_id_loaded) {
        if (s_giphy_random_id[0] == '\0') return ESP_ERR_NOT_FOUND;
        strlcpy(out, s_giphy_random_id, max_len);
        return ESP_OK;
    }

    cJSON *cfg = NULL;
    if (config_store_load(&cfg) != ESP_OK) {
        s_giphy_random_id_loaded = true;
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_random_id");
    if (item && cJSON_IsString(item) && item->valuestring[0]) {
        strlcpy(s_giphy_random_id, item->valuestring, sizeof(s_giphy_random_id));
    }

    s_giphy_random_id_loaded = true;
    cJSON_Delete(cfg);

    if (s_giphy_random_id[0] == '\0') return ESP_ERR_NOT_FOUND;
    strlcpy(out, s_giphy_random_id, max_len);
    return ESP_OK;
}

esp_err_t config_store_delete_giphy_random_id(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON_DeleteItemFromObject(cfg, "giphy_random_id");

    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    if (err == ESP_OK) {
        s_giphy_random_id[0] = '\0';
        s_giphy_random_id_loaded = true;
    }
    return err;
}

void config_store_invalidate_giphy_random_id(void)
{
    s_giphy_random_id_loaded = false;
}

// ============================================================================
// Giphy Cache Size (persisted, cached)
// ============================================================================

#define GIPHY_CACHE_SIZE_DEFAULT 192
#define GIPHY_CACHE_SIZE_MIN     32
#define GIPHY_CACHE_SIZE_MAX     500

static uint32_t s_giphy_cache_size = GIPHY_CACHE_SIZE_DEFAULT;
static bool s_giphy_cache_size_loaded = false;

esp_err_t config_store_set_giphy_cache_size(uint32_t size)
{
    if (size < GIPHY_CACHE_SIZE_MIN || size > GIPHY_CACHE_SIZE_MAX) {
        ESP_LOGE(TAG, "Invalid giphy cache size: %lu (must be %d-%d)",
                 (unsigned long)size, GIPHY_CACHE_SIZE_MIN, GIPHY_CACHE_SIZE_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_cache_size");
    if (item) {
        cJSON_SetNumberValue(item, (double)size);
    } else {
        cJSON_AddNumberToObject(cfg, "giphy_cache_size", (double)size);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_giphy_cache_size = size;
        s_giphy_cache_size_loaded = true;
        ESP_LOGI(TAG, "Giphy cache size saved: %lu", (unsigned long)size);
    }

    return err;
}

uint32_t config_store_get_giphy_cache_size(void)
{
    if (s_giphy_cache_size_loaded) {
        return s_giphy_cache_size;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_giphy_cache_size_loaded = true;
        return s_giphy_cache_size;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_cache_size");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= GIPHY_CACHE_SIZE_MIN && value <= GIPHY_CACHE_SIZE_MAX) {
            s_giphy_cache_size = (uint32_t)value;
        }
    }

    s_giphy_cache_size_loaded = true;
    cJSON_Delete(cfg);
    return s_giphy_cache_size;
}

void config_store_invalidate_giphy_cache_size(void)
{
    s_giphy_cache_size_loaded = false;
}

// ============================================================================
// Giphy Refresh Interval (persisted, cached)
// ============================================================================

#define GIPHY_REFRESH_INTERVAL_DEFAULT 3600

static uint32_t s_giphy_refresh_interval = GIPHY_REFRESH_INTERVAL_DEFAULT;
static bool s_giphy_refresh_interval_loaded = false;

esp_err_t config_store_set_giphy_refresh_interval(uint32_t seconds)
{
    if (seconds < 60 || seconds > 14400) {
        ESP_LOGE(TAG, "Invalid giphy refresh interval: %lu sec", (unsigned long)seconds);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_refresh_interval");
    if (item) {
        cJSON_SetNumberValue(item, (double)seconds);
    } else {
        cJSON_AddNumberToObject(cfg, "giphy_refresh_interval", (double)seconds);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_giphy_refresh_interval = seconds;
        s_giphy_refresh_interval_loaded = true;
        ESP_LOGI(TAG, "Giphy refresh interval saved: %lu seconds", (unsigned long)seconds);
    }

    return err;
}

uint32_t config_store_get_giphy_refresh_interval(void)
{
    if (s_giphy_refresh_interval_loaded) {
        return s_giphy_refresh_interval;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_giphy_refresh_interval_loaded = true;
        return s_giphy_refresh_interval;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_refresh_interval");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= 60 && value <= 14400) {
            s_giphy_refresh_interval = (uint32_t)value;
        }
    }

    s_giphy_refresh_interval_loaded = true;
    cJSON_Delete(cfg);
    return s_giphy_refresh_interval;
}

void config_store_invalidate_giphy_refresh_interval(void)
{
    s_giphy_refresh_interval_loaded = false;
}

// ============================================================================
// Giphy Prefer Downsized (persisted, cached)
// ============================================================================

static bool s_giphy_prefer_downsized = true;   // Default: enabled
static bool s_giphy_prefer_downsized_loaded = false;

esp_err_t config_store_set_giphy_prefer_downsized(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) return err;

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_prefer_downsized");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "giphy_prefer_downsized");
    }
    cJSON_AddBoolToObject(cfg, "giphy_prefer_downsized", enable);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    if (err == ESP_OK) {
        s_giphy_prefer_downsized = enable;
        s_giphy_prefer_downsized_loaded = true;
    }
    return err;
}

bool config_store_get_giphy_prefer_downsized(void)
{
    if (s_giphy_prefer_downsized_loaded) {
        return s_giphy_prefer_downsized;
    }

    cJSON *cfg = NULL;
    if (config_store_load(&cfg) != ESP_OK) {
        s_giphy_prefer_downsized_loaded = true;
        return s_giphy_prefer_downsized;  // Return default (true)
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "giphy_prefer_downsized");
    if (item && cJSON_IsBool(item)) {
        s_giphy_prefer_downsized = cJSON_IsTrue(item);
    }

    s_giphy_prefer_downsized_loaded = true;
    cJSON_Delete(cfg);
    return s_giphy_prefer_downsized;
}

void config_store_invalidate_giphy_prefer_downsized(void)
{
    s_giphy_prefer_downsized_loaded = false;
}

// ============================================================================
// WiFi Recovery Reboot Counters
// ============================================================================

#define NVS_KEY_WIFI_RST_TOT "wifi_rst_tot"
#define NVS_KEY_WIFI_RST_STR "wifi_rst_str"

static uint16_t nvs_read_u16(const char *key)
{
    nvs_handle_t h;
    if (cfg_ensure_nvs(&h) != ESP_OK) return 0;
    uint16_t val = 0;
    nvs_get_u16(h, key, &val);
    nvs_close(h);
    return val;
}

static void nvs_write_u16(const char *key, uint16_t val)
{
    nvs_handle_t h;
    if (cfg_ensure_nvs(&h) != ESP_OK) return;
    nvs_set_u16(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

uint16_t config_store_get_wifi_reboot_total(void)
{
    return nvs_read_u16(NVS_KEY_WIFI_RST_TOT);
}

void config_store_increment_wifi_reboot_total(void)
{
    uint16_t v = nvs_read_u16(NVS_KEY_WIFI_RST_TOT);
    if (v < UINT16_MAX) v++;
    nvs_write_u16(NVS_KEY_WIFI_RST_TOT, v);
}

uint16_t config_store_get_wifi_reboot_streak(void)
{
    return nvs_read_u16(NVS_KEY_WIFI_RST_STR);
}

void config_store_increment_wifi_reboot_streak(void)
{
    uint16_t v = nvs_read_u16(NVS_KEY_WIFI_RST_STR);
    if (v < UINT16_MAX) v++;
    nvs_write_u16(NVS_KEY_WIFI_RST_STR, v);
}

void config_store_reset_wifi_reboot_streak(void)
{
    nvs_write_u16(NVS_KEY_WIFI_RST_STR, 0);
}

void config_store_reset_wifi_reboot_counters(void)
{
    nvs_write_u16(NVS_KEY_WIFI_RST_TOT, 0);
    nvs_write_u16(NVS_KEY_WIFI_RST_STR, 0);
}

// ============================================================================
// Touch Recovery Reboot Counters
// ============================================================================

#define NVS_KEY_TOUCH_RST_TOT "touch_rst_tot"
#define NVS_KEY_TOUCH_RST_STR "touch_rst_str"

uint16_t config_store_get_touch_reboot_total(void)
{
    return nvs_read_u16(NVS_KEY_TOUCH_RST_TOT);
}

void config_store_increment_touch_reboot_total(void)
{
    uint16_t v = nvs_read_u16(NVS_KEY_TOUCH_RST_TOT);
    if (v < UINT16_MAX) v++;
    nvs_write_u16(NVS_KEY_TOUCH_RST_TOT, v);
}

uint16_t config_store_get_touch_reboot_streak(void)
{
    return nvs_read_u16(NVS_KEY_TOUCH_RST_STR);
}

void config_store_increment_touch_reboot_streak(void)
{
    uint16_t v = nvs_read_u16(NVS_KEY_TOUCH_RST_STR);
    if (v < UINT16_MAX) v++;
    nvs_write_u16(NVS_KEY_TOUCH_RST_STR, v);
}

void config_store_reset_touch_reboot_streak(void)
{
    nvs_write_u16(NVS_KEY_TOUCH_RST_STR, 0);
}

void config_store_reset_touch_reboot_counters(void)
{
    nvs_write_u16(NVS_KEY_TOUCH_RST_TOT, 0);
    nvs_write_u16(NVS_KEY_TOUCH_RST_STR, 0);
}
