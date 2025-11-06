#include "storage/kv.h"

#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "storage_kv";

esp_err_t storage_kv_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated and needs to be erased");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");
    ESP_LOGI(TAG, "NVS flash initialized");
    return ESP_OK;
}

void *storage_kv_open_namespace(const char *namespace_name, const char *open_mode)
{
    nvs_handle_t handle = 0;
    nvs_open_mode_t mode = (strcmp(open_mode, "rw") == 0) ? NVS_READWRITE : NVS_READONLY;
    
    esp_err_t ret = nvs_open(namespace_name, mode, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open namespace '%s': %s", namespace_name, esp_err_to_name(ret));
        return NULL;
    }
    
    return (void *)(uintptr_t)handle;
}

void storage_kv_close_namespace(void *handle)
{
    if (handle) {
        nvs_close((nvs_handle_t)(uintptr_t)handle);
    }
}

esp_err_t storage_kv_set_i8(void *handle, const char *key, int8_t value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_i8((nvs_handle_t)(uintptr_t)handle, key, value);
}

esp_err_t storage_kv_get_i8(void *handle, const char *key, int8_t *out_value)
{
    if (!handle || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_i8((nvs_handle_t)(uintptr_t)handle, key, out_value);
}

esp_err_t storage_kv_set_i16(void *handle, const char *key, int16_t value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_i16((nvs_handle_t)(uintptr_t)handle, key, value);
}

esp_err_t storage_kv_get_i16(void *handle, const char *key, int16_t *out_value)
{
    if (!handle || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_i16((nvs_handle_t)(uintptr_t)handle, key, out_value);
}

esp_err_t storage_kv_set_i32(void *handle, const char *key, int32_t value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_i32((nvs_handle_t)(uintptr_t)handle, key, value);
}

esp_err_t storage_kv_get_i32(void *handle, const char *key, int32_t *out_value)
{
    if (!handle || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_i32((nvs_handle_t)(uintptr_t)handle, key, out_value);
}

esp_err_t storage_kv_set_str(void *handle, const char *key, const char *value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_str((nvs_handle_t)(uintptr_t)handle, key, value);
}

esp_err_t storage_kv_get_str(void *handle, const char *key, char *out_value, size_t max_len)
{
    if (!handle || !out_value || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t required_size = max_len;
    esp_err_t ret = nvs_get_str((nvs_handle_t)(uintptr_t)handle, key, out_value, &required_size);
    if (ret == ESP_OK && required_size > max_len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    return ret;
}

esp_err_t storage_kv_set_blob(void *handle, const char *key, const void *value, size_t len)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_blob((nvs_handle_t)(uintptr_t)handle, key, value, len);
}

esp_err_t storage_kv_get_blob(void *handle, const char *key, void *out_value, size_t *max_len)
{
    if (!handle || !out_value || !max_len) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_blob((nvs_handle_t)(uintptr_t)handle, key, out_value, max_len);
}

esp_err_t storage_kv_erase_key(void *handle, const char *key)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_erase_key((nvs_handle_t)(uintptr_t)handle, key);
}

esp_err_t storage_kv_erase_all(void *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_erase_all((nvs_handle_t)(uintptr_t)handle);
}

