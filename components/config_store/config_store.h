#pragma once

#include "cJSON.h"
#include "esp_err.h"

/**
 * @brief Load configuration from NVS
 * 
 * Reads the current config from NVS namespace "appcfg" and returns
 * a deep-copied cJSON object. Caller must free with cJSON_Delete().
 * 
 * If config is missing or corrupt, returns an empty JSON object {}.
 * 
 * @param out_cfg Pointer to receive the cJSON object (must be freed by caller)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_load(cJSON **out_cfg);

/**
 * @brief Save configuration to NVS atomically
 * 
 * Validates that the config is a JSON object and serialized size <= 32 KB.
 * Saves atomically: writes to temp key, validates, then swaps to main key.
 * 
 * @param cfg cJSON object to save (must be a JSON object)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_save(const cJSON *cfg);

/**
 * @brief Get current config as serialized JSON string
 * 
 * Returns a malloc'd string containing the current config JSON.
 * Caller must free the returned string.
 * 
 * @param out_json Pointer to receive the JSON string (must be freed by caller)
 * @param out_len Pointer to receive the string length (excluding null terminator)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_get_serialized(char **out_json, size_t *out_len);

