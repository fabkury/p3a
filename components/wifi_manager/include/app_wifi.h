#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback function type for REST API startup
 * 
 * Called when Wi-Fi STA gets an IP address and REST API is ready.
 * Use this callback to register action handlers with http_api_set_action_handlers().
 */
typedef void (*app_wifi_rest_callback_t)(void);

/**
 * @brief Initialize Wi-Fi station and captive portal
 * 
 * Attempts to connect using saved credentials. If connection fails or no
 * credentials exist, starts a captive portal Soft AP for configuration.
 * 
 * When STA connects and gets an IP, app_state is initialized and REST API
 * is started. The rest_callback is then invoked to allow registration of
 * action handlers.
 * 
 * @param rest_callback Callback invoked when REST API is ready (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t app_wifi_init(app_wifi_rest_callback_t rest_callback);

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

