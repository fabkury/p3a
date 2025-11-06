#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * @brief Initialize NVS flash subsystem.
 * 
 * Must be called before any kv_* functions.
 */
esp_err_t storage_kv_init(void);

/**
 * @brief Open a namespace handle for key-value operations.
 * 
 * @param namespace_name Namespace string (max 15 chars)
 * @param open_mode "rw" or "readonly"
 * @return Handle pointer (NULL on error)
 */
void *storage_kv_open_namespace(const char *namespace_name, const char *open_mode);

/**
 * @brief Close a namespace handle.
 */
void storage_kv_close_namespace(void *handle);

/**
 * @brief Set an 8-bit integer value.
 */
esp_err_t storage_kv_set_i8(void *handle, const char *key, int8_t value);

/**
 * @brief Get an 8-bit integer value.
 */
esp_err_t storage_kv_get_i8(void *handle, const char *key, int8_t *out_value);

/**
 * @brief Set a 16-bit integer value.
 */
esp_err_t storage_kv_set_i16(void *handle, const char *key, int16_t value);

/**
 * @brief Get a 16-bit integer value.
 */
esp_err_t storage_kv_get_i16(void *handle, const char *key, int16_t *out_value);

/**
 * @brief Set a 32-bit integer value.
 */
esp_err_t storage_kv_get_i32(void *handle, const char *key, int32_t *out_value);

/**
 * @brief Get a 32-bit integer value.
 */
esp_err_t storage_kv_set_i32(void *handle, const char *key, int32_t value);

/**
 * @brief Set a string value.
 */
esp_err_t storage_kv_set_str(void *handle, const char *key, const char *value);

/**
 * @brief Get a string value.
 * 
 * @param handle Namespace handle
 * @param key Key name
 * @param out_value Buffer to store value
 * @param max_len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if key doesn't exist
 */
esp_err_t storage_kv_get_str(void *handle, const char *key, char *out_value, size_t max_len);

/**
 * @brief Set a blob value.
 */
esp_err_t storage_kv_set_blob(void *handle, const char *key, const void *value, size_t len);

/**
 * @brief Get a blob value.
 * 
 * @param handle Namespace handle
 * @param key Key name
 * @param out_value Buffer to store value
 * @param max_len Maximum length of buffer (will be updated with actual length on success)
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if key doesn't exist
 */
esp_err_t storage_kv_get_blob(void *handle, const char *key, void *out_value, size_t *max_len);

/**
 * @brief Erase a key.
 */
esp_err_t storage_kv_erase_key(void *handle, const char *key);

/**
 * @brief Erase all keys in a namespace.
 */
esp_err_t storage_kv_erase_all(void *handle);

