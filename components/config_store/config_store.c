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

    ESP_LOGI(TAG, "Config saved successfully (%zu bytes)", len);
    return ESP_OK;
}

esp_err_t config_store_set_rotation(screen_rotation_t rotation)
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
        cJSON_SetNumberValue(rotation_item, (double)rotation);
    } else {
        cJSON_AddNumberToObject(cfg, "rotation", (double)rotation);
    }
    
    // Save updated config
    err = config_store_save(cfg);
    cJSON_Delete(cfg);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Rotation saved to config: %d degrees", rotation);
    }
    
    return err;
}

screen_rotation_t config_store_get_rotation(void)
{
    cJSON *cfg = NULL;
    esp_err_t err = config_store_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load config for rotation, using default");
        return ROTATION_0;
    }
    
    screen_rotation_t rotation = ROTATION_0;
    cJSON *rotation_item = cJSON_GetObjectItem(cfg, "rotation");
    if (rotation_item && cJSON_IsNumber(rotation_item)) {
        int value = (int)cJSON_GetNumberValue(rotation_item);
        // Validate rotation value
        if (value == 0 || value == 90 || value == 180 || value == 270) {
            rotation = (screen_rotation_t)value;
        } else {
            ESP_LOGW(TAG, "Invalid rotation value in config: %d, using default", value);
        }
    }
    
    cJSON_Delete(cfg);
    return rotation;
}


