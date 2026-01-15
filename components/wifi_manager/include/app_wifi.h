// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Wi-Fi station and captive portal
 * 
 * Attempts to connect using saved credentials. If connection fails or no
 * credentials exist, starts a captive portal Soft AP for configuration.
 * 
 * When STA connects and gets an IP, REST API is started.
 * @return ESP_OK on success
 */
esp_err_t app_wifi_init(void);

/**
 * @brief Get saved Wi-Fi SSID from NVS
 * 
 * @param ssid Buffer to store SSID (must be at least 33 bytes)
 * @param max_len Maximum length of ssid buffer
 * @return ESP_OK on success, error code if SSID not found or read failed
 */
esp_err_t app_wifi_get_saved_ssid(char *ssid, size_t max_len);

/**
 * @brief Erase saved Wi-Fi credentials from NVS
 * 
 * @return ESP_OK on success
 */
esp_err_t app_wifi_erase_credentials(void);

/**
 * @brief Check if captive portal is currently active
 * 
 * @return true if captive portal is running, false otherwise
 */
bool app_wifi_is_captive_portal_active(void);

/**
 * @brief Get the current local IPv4 address
 * 
 * Returns the IPv4 address of either the AP (if in captive portal mode)
 * or the STA interface (if connected to WiFi network).
 * 
 * Note: Currently only supports IPv4. IPv6 support may be added in future.
 * 
 * @param ip_str Buffer to store IP address string (e.g., "192.168.4.1")
 * @param max_len Maximum length of ip_str buffer (minimum 16 bytes recommended)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no IP available, ESP_ERR_INVALID_ARG if invalid parameters
 */
esp_err_t app_wifi_get_local_ip(char *ip_str, size_t max_len);

#ifdef __cplusplus
}
#endif

