/**
 * @file slave_ota.h
 * @brief ESP32-C6 Co-processor OTA Update via ESP-Hosted
 * 
 * This component updates the ESP32-C6 co-processor firmware using
 * ESP-Hosted transport OTA. The slave firmware is stored in a
 * dedicated partition and flashed to the C6 at boot if needed.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check and update ESP32-C6 co-processor firmware if needed
 * 
 * This function should be called early in boot, after ESP-Hosted
 * transport is initialized. It:
 * 1. Gets the current co-processor firmware version
 * 2. Compares with the embedded firmware version
 * 3. Performs OTA update if versions differ
 * 
 * @return ESP_OK on success (no update needed or update completed)
 *         ESP_FAIL if update failed
 *         ESP_ERR_NOT_FOUND if slave firmware partition not found
 */
esp_err_t slave_ota_check_and_update(void);

/**
 * @brief Get the embedded slave firmware version
 * 
 * @param[out] major Major version number
 * @param[out] minor Minor version number  
 * @param[out] patch Patch version number
 * @return ESP_OK on success
 */
esp_err_t slave_ota_get_embedded_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

#ifdef __cplusplus
}
#endif

