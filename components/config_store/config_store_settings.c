// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "config_store_internal.h"

static const char *TAG = "CFG";

// ============================================================================
// Background color cache
// ============================================================================

static uint8_t s_bg_r = 0;
static uint8_t s_bg_g = 0;
static uint8_t s_bg_b = 0;
static bool s_bg_loaded = false;
static uint32_t s_bg_generation = 0;

void cfg_bg_apply_from_cfg(const cJSON *cfg)
{
    // Default: black
    uint8_t r = 0, g = 0, b = 0;

    if (cfg && cJSON_IsObject((cJSON *)cfg)) {
        const cJSON *bg = cJSON_GetObjectItem((cJSON *)cfg, "background_color");
        if (bg && cJSON_IsObject((cJSON *)bg)) {
            r = cfg_clamp_u8_num(cJSON_GetObjectItem((cJSON *)bg, "r"), 0);
            g = cfg_clamp_u8_num(cJSON_GetObjectItem((cJSON *)bg, "g"), 0);
            b = cfg_clamp_u8_num(cJSON_GetObjectItem((cJSON *)bg, "b"), 0);
        }
    }

    const bool changed = (!s_bg_loaded) || (r != s_bg_r) || (g != s_bg_g) || (b != s_bg_b);
    s_bg_r = r;
    s_bg_g = g;
    s_bg_b = b;
    if (changed) {
        s_bg_generation++;
        ESP_LOGI(TAG, "Background color updated: r=%u g=%u b=%u (gen=%lu)",
                 (unsigned)s_bg_r, (unsigned)s_bg_g, (unsigned)s_bg_b, (unsigned long)s_bg_generation);
    }
    s_bg_loaded = true;
}

// ============================================================================
// FPS display cache
// ============================================================================

static bool s_show_fps = false;  // Default: OFF
static bool s_show_fps_loaded = false;

void cfg_show_fps_apply_from_cfg(const cJSON *cfg)
{
    bool show_fps = false;  // Default: OFF

    if (cfg && cJSON_IsObject((cJSON *)cfg)) {
        const cJSON *item = cJSON_GetObjectItem((cJSON *)cfg, "show_fps");
        if (item && cJSON_IsBool((cJSON *)item)) {
            show_fps = cJSON_IsTrue((cJSON *)item);
        }
    }

    if (!s_show_fps_loaded || show_fps != s_show_fps) {
        ESP_LOGI(TAG, "Show FPS updated: %s", show_fps ? "ON" : "OFF");
    }
    s_show_fps = show_fps;
    s_show_fps_loaded = true;
}

// ============================================================================
// Max speed playback cache
// ============================================================================

static bool s_max_speed_playback = false;  // Default: OFF
static bool s_max_speed_playback_loaded = false;

void cfg_max_speed_playback_apply_from_cfg(const cJSON *cfg)
{
    bool max_speed = false;  // Default: OFF

    if (cfg && cJSON_IsObject((cJSON *)cfg)) {
        const cJSON *item = cJSON_GetObjectItem((cJSON *)cfg, "max_speed_playback");
        if (item && cJSON_IsBool((cJSON *)item)) {
            max_speed = cJSON_IsTrue((cJSON *)item);
        }
    }

    if (!s_max_speed_playback_loaded || max_speed != s_max_speed_playback) {
        ESP_LOGI(TAG, "Max speed playback updated: %s", max_speed ? "ON" : "OFF");
    }
    s_max_speed_playback = max_speed;
    s_max_speed_playback_loaded = true;
}

// ============================================================================
// Rotation
// ============================================================================

esp_err_t config_store_set_rotation(uint16_t rotation_degrees)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for rotation update");
        return err;
    }

    // Add or update rotation field
    cJSON *rotation_item = cJSON_GetObjectItem(cfg, "rotation");
    if (rotation_item) {
        cJSON_SetNumberValue(rotation_item, (double)rotation_degrees);
    } else {
        cJSON_AddNumberToObject(cfg, "rotation", (double)rotation_degrees);
    }

    // Save updated config
    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Rotation saved to config: %u degrees", (unsigned)rotation_degrees);
    }

    return err;
}

uint16_t config_store_get_rotation(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load config for rotation, using default");
        return 0;
    }

    uint16_t rotation = 0;
    cJSON *rotation_item = cJSON_GetObjectItem(cfg, "rotation");
    if (rotation_item && cJSON_IsNumber(rotation_item)) {
        int value = (int)cJSON_GetNumberValue(rotation_item);
        // Validate rotation value
        if (value == 0 || value == 90 || value == 180 || value == 270) {
            rotation = (uint16_t)value;
        } else {
            ESP_LOGW(TAG, "Invalid rotation value in config: %d, using default", value);
        }
    }

    cJSON_Delete(cfg);
    return rotation;
}

// ============================================================================
// Playback Settings
// ============================================================================

esp_err_t config_store_set_randomize_playlist(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "randomize_playlist");
    if (item) {
        // cJSON doesn't have SetBoolValue, so delete and re-add
        cJSON_DeleteItemFromObject(cfg, "randomize_playlist");
    }
    cJSON_AddBoolToObject(cfg, "randomize_playlist", enable);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Randomize playlist saved: %s", enable ? "ON" : "OFF");
    }

    return err;
}

bool config_store_get_randomize_playlist(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return false;  // Default: OFF
    }

    bool enable = false;
    cJSON *item = cJSON_GetObjectItem(cfg, "randomize_playlist");
    if (item && cJSON_IsBool(item)) {
        enable = cJSON_IsTrue(item);
    }

    cJSON_Delete(cfg);
    return enable;
}

// ============================================================================
// View Acknowledgment
// ============================================================================

static bool s_view_ack = false;
static bool s_view_ack_loaded = false;

esp_err_t config_store_set_view_ack(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "view_ack");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "view_ack");
    }
    cJSON_AddBoolToObject(cfg, "view_ack", enable);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_view_ack = enable;
        s_view_ack_loaded = true;
    }

    return err;
}

bool config_store_get_view_ack(void)
{
    if (s_view_ack_loaded) {
        return s_view_ack;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_view_ack_loaded = true;
        return s_view_ack;  // Default: OFF (fire-and-forget)
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "view_ack");
    if (item && cJSON_IsBool(item)) {
        s_view_ack = cJSON_IsTrue(item);
    }

    s_view_ack_loaded = true;
    cJSON_Delete(cfg);
    return s_view_ack;
}

void config_store_invalidate_view_ack(void)
{
    s_view_ack_loaded = false;
}

// ============================================================================
// Dwell Time
// ============================================================================

static uint32_t s_dwell_time = 30000;
static bool s_dwell_time_loaded = false;

esp_err_t config_store_set_dwell_time(uint32_t dwell_time_ms)
{
    // 0 is allowed and means "auto-swap disabled"
    if (dwell_time_ms > 86400000) {  // Max 24 hours
        ESP_LOGE(TAG, "Invalid dwell time: %lu ms (max 86400000)", dwell_time_ms);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "dwell_time_ms");
    if (item) {
        cJSON_SetNumberValue(item, (double)dwell_time_ms);
    } else {
        cJSON_AddNumberToObject(cfg, "dwell_time_ms", (double)dwell_time_ms);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_dwell_time = dwell_time_ms;
        s_dwell_time_loaded = true;
        ESP_LOGI(TAG, "Dwell time saved: %lu ms", dwell_time_ms);
    }

    return err;
}

uint32_t config_store_get_dwell_time(void)
{
    if (s_dwell_time_loaded) {
        return s_dwell_time;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_dwell_time_loaded = true;
        return s_dwell_time;  // Default: 30 seconds
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "dwell_time_ms");
    if (item && cJSON_IsNumber(item)) {
        int value = (int)cJSON_GetNumberValue(item);
        if (value >= 0 && value <= 86400000) {  // 0 = disabled, max 24h
            s_dwell_time = (uint32_t)value;
        }
    }

    s_dwell_time_loaded = true;
    cJSON_Delete(cfg);
    return s_dwell_time;
}

void config_store_invalidate_dwell_time(void)
{
    s_dwell_time_loaded = false;
}

// ============================================================================
// Background color (persisted)
// ============================================================================

esp_err_t config_store_set_background_color(uint8_t r, uint8_t g, uint8_t b)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for background_color update");
        return err;
    }

    cJSON *bg = cJSON_GetObjectItem(cfg, "background_color");
    if (bg && !cJSON_IsObject(bg)) {
        cJSON_DeleteItemFromObject(cfg, "background_color");
        bg = NULL;
    }
    if (!bg) {
        bg = cJSON_CreateObject();
        if (!bg) {
            cJSON_Delete(cfg);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(cfg, "background_color", bg);
    }

    // Set r/g/b (replace if exists)
    cJSON *ri = cJSON_GetObjectItem(bg, "r");
    if (ri) cJSON_SetNumberValue(ri, (double)r);
    else cJSON_AddNumberToObject(bg, "r", (double)r);

    cJSON *gi = cJSON_GetObjectItem(bg, "g");
    if (gi) cJSON_SetNumberValue(gi, (double)g);
    else cJSON_AddNumberToObject(bg, "g", (double)g);

    cJSON *bi = cJSON_GetObjectItem(bg, "b");
    if (bi) cJSON_SetNumberValue(bi, (double)b);
    else cJSON_AddNumberToObject(bg, "b", (double)b);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    return err;
}

void config_store_get_background_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!s_bg_loaded) {
        // Lazy-load from NVS once.
        cJSON *cfg = NULL;
        if (config_store_load(&cfg) == ESP_OK) {
            cJSON_Delete(cfg);
        } else {
            // Keep defaults if load fails.
            cfg_bg_apply_from_cfg(NULL);
        }
    }

    if (r) *r = s_bg_r;
    if (g) *g = s_bg_g;
    if (b) *b = s_bg_b;
}

uint32_t config_store_get_background_color_generation(void)
{
    if (!s_bg_loaded) {
        uint8_t r, g, b;
        config_store_get_background_color(&r, &g, &b);
    }
    return s_bg_generation;
}

// ============================================================================
// FPS Display (persisted)
// ============================================================================

esp_err_t config_store_set_show_fps(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "show_fps");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "show_fps");
    }
    cJSON_AddBoolToObject(cfg, "show_fps", enable);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_show_fps = enable;
        s_show_fps_loaded = true;
        ESP_LOGI(TAG, "Show FPS saved: %s", enable ? "ON" : "OFF");
    }

    return err;
}

bool config_store_get_show_fps(void)
{
    if (s_show_fps_loaded) {
        return s_show_fps;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_show_fps_loaded = true;
        return s_show_fps;  // Return default (false)
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "show_fps");
    if (item && cJSON_IsBool(item)) {
        s_show_fps = cJSON_IsTrue(item);
    }
    // If not present in config, default is false (s_show_fps already initialized to false)

    s_show_fps_loaded = true;
    cJSON_Delete(cfg);
    return s_show_fps;
}

// ============================================================================
// Max Speed Playback (persisted)
// ============================================================================

esp_err_t config_store_set_max_speed_playback(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "max_speed_playback");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "max_speed_playback");
    }
    cJSON_AddBoolToObject(cfg, "max_speed_playback", enable);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_max_speed_playback = enable;
        s_max_speed_playback_loaded = true;
        ESP_LOGI(TAG, "Max speed playback saved: %s", enable ? "ON" : "OFF");
    }

    return err;
}

bool config_store_get_max_speed_playback(void)
{
    if (s_max_speed_playback_loaded) {
        return s_max_speed_playback;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_max_speed_playback_loaded = true;
        return s_max_speed_playback;  // Return default (false)
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "max_speed_playback");
    if (item && cJSON_IsBool(item)) {
        s_max_speed_playback = cJSON_IsTrue(item);
    }
    // If not present in config, default is false

    s_max_speed_playback_loaded = true;
    cJSON_Delete(cfg);
    return s_max_speed_playback;
}

// ============================================================================
// Refresh Interval (persisted)
// ============================================================================

static uint32_t s_refresh_interval_sec = 3600;
static bool s_refresh_interval_sec_loaded = false;

esp_err_t config_store_set_refresh_interval_sec(uint32_t interval_sec)
{
    // Validate reasonable range: 60 seconds to 24 hours
    if (interval_sec < 60 || interval_sec > 86400) {
        ESP_LOGE(TAG, "Invalid refresh interval: %lu sec (must be 60-86400)",
                 (unsigned long)interval_sec);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "refresh_interval_sec");
    if (item) {
        cJSON_SetNumberValue(item, (double)interval_sec);
    } else {
        cJSON_AddNumberToObject(cfg, "refresh_interval_sec", (double)interval_sec);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_refresh_interval_sec = interval_sec;
        s_refresh_interval_sec_loaded = true;
        ESP_LOGI(TAG, "Refresh interval saved: %lu seconds", (unsigned long)interval_sec);
    }

    return err;
}

uint32_t config_store_get_refresh_interval_sec(void)
{
    if (s_refresh_interval_sec_loaded) {
        return s_refresh_interval_sec;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_refresh_interval_sec_loaded = true;
        return s_refresh_interval_sec;  // Default: 1 hour
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "refresh_interval_sec");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= 60 && value <= 86400) {
            s_refresh_interval_sec = (uint32_t)value;
        }
    }

    s_refresh_interval_sec_loaded = true;
    cJSON_Delete(cfg);
    return s_refresh_interval_sec;
}

void config_store_invalidate_refresh_interval_sec(void)
{
    s_refresh_interval_sec_loaded = false;
}

// ============================================================================
// Refresh Allow Override (runtime only, not persisted)
// ============================================================================

static bool s_refresh_allow_override = false;

void config_store_set_refresh_allow_override(bool allow)
{
    s_refresh_allow_override = allow;
    ESP_LOGI(TAG, "Refresh allow override: %s", allow ? "ON" : "OFF");
}

bool config_store_get_refresh_allow_override(void)
{
    return s_refresh_allow_override;
}

// ============================================================================
// SD Card Root Folder (persisted, requires reboot)
// ============================================================================

esp_err_t config_store_set_sdcard_root(const char *root_path)
{
    if (!root_path || root_path[0] == '\0') {
        ESP_LOGE(TAG, "Invalid SD root path");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "sdcard_root");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "sdcard_root");
    }
    cJSON_AddStringToObject(cfg, "sdcard_root", root_path);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD root path saved: %s (reboot required)", root_path);
    }

    return err;
}

esp_err_t config_store_get_sdcard_root(char **out_path)
{
    if (!out_path) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_path = NULL;

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "sdcard_root");
    if (item && cJSON_IsString(item)) {
        const char *val = cJSON_GetStringValue(item);
        if (val && val[0] != '\0') {
            *out_path = strdup(val);
            if (!*out_path) {
                cJSON_Delete(cfg);
                return ESP_ERR_NO_MEM;
            }
            cJSON_Delete(cfg);
            return ESP_OK;
        }
    }

    cJSON_Delete(cfg);
    return ESP_ERR_NOT_FOUND;
}

// ============================================================================
// Channel Cache Size (persisted, with in-memory caching)
// ============================================================================

#define CHANNEL_CACHE_SIZE_DEFAULT 2048
#define CHANNEL_CACHE_SIZE_MIN     32
#define CHANNEL_CACHE_SIZE_MAX     4096

static uint32_t s_channel_cache_size = CHANNEL_CACHE_SIZE_DEFAULT;
static bool s_channel_cache_size_loaded = false;

esp_err_t config_store_set_channel_cache_size(uint32_t size)
{
    // Validate range
    if (size < CHANNEL_CACHE_SIZE_MIN || size > CHANNEL_CACHE_SIZE_MAX) {
        ESP_LOGE(TAG, "Invalid channel cache size: %lu (must be %d-%d)",
                 (unsigned long)size, CHANNEL_CACHE_SIZE_MIN, CHANNEL_CACHE_SIZE_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "channel_cache_size");
    if (item) {
        cJSON_SetNumberValue(item, (double)size);
    } else {
        cJSON_AddNumberToObject(cfg, "channel_cache_size", (double)size);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_channel_cache_size = size;
        s_channel_cache_size_loaded = true;
        ESP_LOGI(TAG, "Channel cache size saved: %lu", (unsigned long)size);
    }

    return err;
}

uint32_t config_store_get_channel_cache_size(void)
{
    if (s_channel_cache_size_loaded) {
        return s_channel_cache_size;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_channel_cache_size_loaded = true;
        return s_channel_cache_size;  // Return default
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "channel_cache_size");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= CHANNEL_CACHE_SIZE_MIN && value <= CHANNEL_CACHE_SIZE_MAX) {
            s_channel_cache_size = (uint32_t)value;
        }
    }

    s_channel_cache_size_loaded = true;
    cJSON_Delete(cfg);
    return s_channel_cache_size;
}

// ============================================================================
// Processing Notification Settings (persisted, with in-memory caching)
// ============================================================================

#define PROC_NOTIF_SIZE_DEFAULT 64
#define PROC_NOTIF_SIZE_MIN     16
#define PROC_NOTIF_SIZE_MAX     256

static bool s_proc_notif_enabled = true;  // Default: ON
static bool s_proc_notif_enabled_loaded = false;
static uint16_t s_proc_notif_size = PROC_NOTIF_SIZE_DEFAULT;
static bool s_proc_notif_size_loaded = false;

esp_err_t config_store_set_proc_notif_enabled(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "proc_notif_enabled");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "proc_notif_enabled");
    }
    cJSON_AddBoolToObject(cfg, "proc_notif_enabled", enable);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_proc_notif_enabled = enable;
        s_proc_notif_enabled_loaded = true;
        ESP_LOGI(TAG, "Processing notification enabled: %s", enable ? "ON" : "OFF");
    }

    return err;
}

bool config_store_get_proc_notif_enabled(void)
{
    if (s_proc_notif_enabled_loaded) {
        return s_proc_notif_enabled;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_proc_notif_enabled_loaded = true;
        return s_proc_notif_enabled;  // Return default (true)
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "proc_notif_enabled");
    if (item && cJSON_IsBool(item)) {
        s_proc_notif_enabled = cJSON_IsTrue(item);
    }
    // If not present in config, default is true (s_proc_notif_enabled already initialized to true)

    s_proc_notif_enabled_loaded = true;
    cJSON_Delete(cfg);
    return s_proc_notif_enabled;
}

esp_err_t config_store_set_proc_notif_size(uint16_t size)
{
    // Normalize size: 0 = disabled, 1-15 -> 16, >256 -> 256
    if (size != 0) {
        if (size < PROC_NOTIF_SIZE_MIN) {
            size = PROC_NOTIF_SIZE_MIN;
        } else if (size > PROC_NOTIF_SIZE_MAX) {
            size = PROC_NOTIF_SIZE_MAX;
        }
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "proc_notif_size");
    if (item) {
        cJSON_SetNumberValue(item, (double)size);
    } else {
        cJSON_AddNumberToObject(cfg, "proc_notif_size", (double)size);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        s_proc_notif_size = size;
        s_proc_notif_size_loaded = true;
        ESP_LOGI(TAG, "Processing notification size saved: %u%s",
                 (unsigned)size, size == 0 ? " (disabled)" : "");
    }

    return err;
}

uint16_t config_store_get_proc_notif_size(void)
{
    if (s_proc_notif_size_loaded) {
        return s_proc_notif_size;
    }

    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        s_proc_notif_size_loaded = true;
        return s_proc_notif_size;  // Return default
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "proc_notif_size");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        // Accept 0 (disabled) or 16-256
        if (value == 0 || (value >= PROC_NOTIF_SIZE_MIN && value <= PROC_NOTIF_SIZE_MAX)) {
            s_proc_notif_size = (uint16_t)value;
        }
    }

    s_proc_notif_size_loaded = true;
    cJSON_Delete(cfg);
    return s_proc_notif_size;
}

// ============================================================================
// Channel Select Mode
// ============================================================================

esp_err_t config_store_set_channel_select_mode(uint8_t mode)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "channel_select_mode");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "channel_select_mode");
    }
    cJSON_AddNumberToObject(cfg, "channel_select_mode", (double)mode);

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Channel select mode saved: %u", (unsigned)mode);
    }

    return err;
}

uint8_t config_store_get_channel_select_mode(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return 0;  // Default: SWRR
    }

    uint8_t mode = 1;  // Default: Stochastic
    cJSON *item = cJSON_GetObjectItem(cfg, "channel_select_mode");
    if (item && cJSON_IsNumber(item)) {
        int val = (int)cJSON_GetNumberValue(item);
        if (val >= 0 && val <= 1) {
            mode = (uint8_t)val;
        }
    }

    cJSON_Delete(cfg);
    return mode;
}

// ============================================================================
// Device Name (persisted)
// ============================================================================

/**
 * Validate device name: [a-z0-9-], max 16 chars, no leading/trailing hyphen.
 * Empty string is allowed (clears the name).
 */
static bool validate_device_name(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0) return true;  // Empty = clear
    if (len > CONFIG_STORE_MAX_DEVICE_NAME_LEN) return false;
    if (name[0] == '-' || name[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t config_store_set_device_name(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!validate_device_name(name)) return ESP_ERR_INVALID_ARG;
    return cfg_set_string("device_name", name);
}

esp_err_t config_store_get_device_name(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    cfg_get_string("device_name", "", out, max_len);
    return ESP_OK;
}

esp_err_t config_store_get_hostname(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;

    char device_name[CONFIG_STORE_MAX_DEVICE_NAME_LEN + 1];
    config_store_get_device_name(device_name, sizeof(device_name));

    if (device_name[0] == '\0') {
        strlcpy(out, "p3a", max_len);
    } else {
        snprintf(out, max_len, "p3a-%s", device_name);
    }
    return ESP_OK;
}
