// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SNTP time synchronization
 * 
 * Starts SNTP client to synchronize system time.
 * Should be called after WiFi is connected.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sntp_sync_init(void);

/**
 * @brief Check if time is synchronized
 * 
 * @return true if time is synchronized, false otherwise
 */
bool sntp_sync_is_synchronized(void);

/**
 * @brief Get current time as ISO 8601 string
 * 
 * Formats current time as "YYYY-MM-DDTHH:MM:SSZ"
 * 
 * @param buf Buffer to receive the timestamp string (must be at least 32 bytes)
 * @param len Maximum length of buffer
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not synchronized, error code otherwise
 */
esp_err_t sntp_sync_get_iso8601(char *buf, size_t len);

/**
 * @brief Stop SNTP synchronization
 * 
 * Stops the SNTP client and frees resources.
 * Can be restarted with sntp_sync_init().
 */
void sntp_sync_stop(void);

#ifdef __cplusplus
}
#endif

