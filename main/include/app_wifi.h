#pragma once

#include "esp_err.h"
#include <stddef.h>

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

