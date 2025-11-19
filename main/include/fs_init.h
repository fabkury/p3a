#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and mount SPIFFS filesystem
 * 
 * Mounts the SPIFFS partition labeled "storage" at /spiffs.
 * Should be called during system initialization.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fs_init(void);

/**
 * @brief Check if SPIFFS is mounted
 * 
 * @return true if mounted, false otherwise
 */
bool fs_is_mounted(void);

#ifdef __cplusplus
}
#endif

