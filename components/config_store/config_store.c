#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CFG";

#define NAMESPACE "appcfg"
#define KEY_CUR   "cfg"
#define KEY_NEW   "cfg_new"
#define MAX_JSON  (32 * 1024)

// ============================================================================
// Background color cache
// ============================================================================

static uint8_t s_bg_r = 0;
static uint8_t s_bg_g = 0;
static uint8_t s_bg_b = 0;
static bool s_bg_loaded = false;
static uint32_t s_bg_generation = 0;

static uint8_t clamp_u8_num(const cJSON *n, uint8_t def)
{
    if (!n || !cJSON_IsNumber((cJSON *)n)) {
        return def;
    }
    double v = cJSON_GetNumberValue((cJSON *)n);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static void bg_apply_from_cfg(const cJSON *cfg)
{
    // Default: black
    uint8_t r = 0, g = 0, b = 0;

    if (cfg && cJSON_IsObject((cJSON *)cfg)) {
        const cJSON *bg = cJSON_GetObjectItem((cJSON *)cfg, "background_color");
        if (bg && cJSON_IsObject((cJSON *)bg)) {
            r = clamp_u8_num(cJSON_GetObjectItem((cJSON *)bg, "r"), 0);
            g = clamp_u8_num(cJSON_GetObjectItem((cJSON *)bg, "g"), 0);
            b = clamp_u8_num(cJSON_GetObjectItem((cJSON *)bg, "b"), 0);
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

static bool s_max_speed_playback = false;  // Default: OFF
static bool s_max_speed_playback_loaded = false;

static void show_fps_apply_from_cfg(const cJSON *cfg)
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

static void max_speed_playback_apply_from_cfg(const cJSON *cfg)
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

static esp_err_t ensure_nvs(nvs_handle_t *h) {
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, h);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        // NVS already initialized in app_main, but safe to call again
        nvs_flash_init();
        err = nvs_open(NAMESPACE, NVS_READWRITE, h);
    }
    return err;
}

esp_err_t config_store_get_serialized(char **out_json, size_t *out_len) {
    nvs_handle_t h;
    esp_err_t err = ensure_nvs(&h);
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
    bg_apply_from_cfg(o);
    show_fps_apply_from_cfg(o);
    max_speed_playback_apply_from_cfg(o);

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
    esp_err_t err = ensure_nvs(&h);
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
    bg_apply_from_cfg(cfg);
    show_fps_apply_from_cfg(cfg);
    max_speed_playback_apply_from_cfg(cfg);

    ESP_LOGI(TAG, "Config saved successfully (%zu bytes)", len);
    return ESP_OK;
}

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
// Playlist Settings
// ============================================================================

esp_err_t config_store_set_pe(uint32_t pe)
{
    if (pe > 1023) {
        ESP_LOGE(TAG, "Invalid PE value: %lu (max 1023)", pe);
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    
    cJSON *pe_item = cJSON_GetObjectItem(cfg, "pe");
    if (pe_item) {
        cJSON_SetNumberValue(pe_item, (double)pe);
    } else {
        cJSON_AddNumberToObject(cfg, "pe", (double)pe);
    }
    
    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PE saved to config: %lu", pe);
    }
    
    return err;
}

uint32_t config_store_get_pe(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return 8;  // Default
    }
    
    uint32_t pe = 8;
    cJSON *pe_item = cJSON_GetObjectItem(cfg, "pe");
    if (pe_item && cJSON_IsNumber(pe_item)) {
        int value = (int)cJSON_GetNumberValue(pe_item);
        if (value >= 0 && value <= 1023) {
            pe = (uint32_t)value;
        }
    }
    
    cJSON_Delete(cfg);
    return pe;
}

esp_err_t config_store_set_play_order(uint8_t order)
{
    if (order > 2) {
        ESP_LOGE(TAG, "Invalid play order: %u", order);
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    
    cJSON *order_item = cJSON_GetObjectItem(cfg, "play_order");
    if (order_item) {
        cJSON_SetNumberValue(order_item, (double)order);
    } else {
        cJSON_AddNumberToObject(cfg, "play_order", (double)order);
    }
    
    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Play order saved: %u", order);
    }
    
    return err;
}

uint8_t config_store_get_play_order(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return 0;  // Default: server order
    }
    
    uint8_t order = 0;
    cJSON *order_item = cJSON_GetObjectItem(cfg, "play_order");
    if (order_item && cJSON_IsNumber(order_item)) {
        int value = (int)cJSON_GetNumberValue(order_item);
        if (value >= 0 && value <= 2) {
            order = (uint8_t)value;
        }
    }
    
    cJSON_Delete(cfg);
    return order;
}

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

esp_err_t config_store_set_live_mode(bool enable)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    
    cJSON *item = cJSON_GetObjectItem(cfg, "live_mode");
    if (item) {
        cJSON_DeleteItemFromObject(cfg, "live_mode");
    }
    cJSON_AddBoolToObject(cfg, "live_mode", enable);
    
    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Live mode saved: %s", enable ? "ON" : "OFF");
    }
    
    return err;
}

bool config_store_get_live_mode(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return false;  // Default: OFF
    }
    
    bool enable = false;
    cJSON *item = cJSON_GetObjectItem(cfg, "live_mode");
    if (item && cJSON_IsBool(item)) {
        enable = cJSON_IsTrue(item);
    }
    
    cJSON_Delete(cfg);
    return enable;
}

esp_err_t config_store_set_dwell_time(uint32_t dwell_time_ms)
{
    // 0 is allowed and means "global override disabled"
    if (dwell_time_ms > 100000000) {  // Max ~27 hours
        ESP_LOGE(TAG, "Invalid dwell time: %lu ms", dwell_time_ms);
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
        ESP_LOGI(TAG, "Dwell time saved: %lu ms", dwell_time_ms);
    }
    
    return err;
}

uint32_t config_store_get_dwell_time(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return 0;  // Default: disabled
    }
    
    uint32_t dwell_time = 0;
    cJSON *item = cJSON_GetObjectItem(cfg, "dwell_time_ms");
    if (item && cJSON_IsNumber(item)) {
        int value = (int)cJSON_GetNumberValue(item);
        if (value >= 0 && value <= 100000000) {
            dwell_time = (uint32_t)value;
        }
    }
    
    cJSON_Delete(cfg);
    return dwell_time;
}

esp_err_t config_store_set_global_seed(uint32_t seed)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *item = cJSON_GetObjectItem(cfg, "global_seed");
    if (item) {
        cJSON_SetNumberValue(item, (double)seed);
    } else {
        cJSON_AddNumberToObject(cfg, "global_seed", (double)seed);
    }

    err = config_store_save(cfg);
    cJSON_Delete(cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Global seed saved: %lu", (unsigned long)seed);
    }

    return err;
}

uint32_t config_store_get_global_seed(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return 0xFAB;
    }

    uint32_t seed = 0xFAB;
    cJSON *item = cJSON_GetObjectItem(cfg, "global_seed");
    if (item && cJSON_IsNumber(item)) {
        double v = cJSON_GetNumberValue(item);
        if (v >= 0) {
            seed = (uint32_t)v;
        }
    }

    cJSON_Delete(cfg);
    return seed;
}

// Runtime-only effective seed (not persisted)
static uint32_t s_effective_seed = 0;
static bool s_effective_seed_set = false;

void config_store_set_effective_seed(uint32_t seed)
{
    s_effective_seed = seed;
    s_effective_seed_set = true;
    ESP_LOGI(TAG, "Effective seed set to: 0x%08x", seed);
}

uint32_t config_store_get_effective_seed(void)
{
    if (s_effective_seed_set) {
        return s_effective_seed;
    }
    
    // Default to master seed if not explicitly set
    return config_store_get_global_seed();
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
            bg_apply_from_cfg(NULL);
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
        ESP_LOGI(TAG, "Refresh interval saved: %lu seconds", (unsigned long)interval_sec);
    }
    
    return err;
}

uint32_t config_store_get_refresh_interval_sec(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        return 3600;  // Default: 1 hour
    }
    
    uint32_t interval_sec = 3600;  // Default: 1 hour
    cJSON *item = cJSON_GetObjectItem(cfg, "refresh_interval_sec");
    if (item && cJSON_IsNumber(item)) {
        double value = cJSON_GetNumberValue(item);
        if (value >= 60 && value <= 86400) {
            interval_sec = (uint32_t)value;
        }
    }
    
    cJSON_Delete(cfg);
    return interval_sec;
}


