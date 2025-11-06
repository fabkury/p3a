#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi connection state
 */
typedef enum {
    NET_WIFI_STATE_DISCONNECTED = 0,
    NET_WIFI_STATE_CONNECTING,
    NET_WIFI_STATE_CONNECTED,
    NET_WIFI_STATE_PROVISIONING,
} net_wifi_state_t;

/**
 * @brief Initialize the networking subsystem
 * 
 * This initializes the Wi-Fi stack using esp_wifi_remote and esp_hosted.
 * Must be called before any other networking functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t net_init(void);

/**
 * @brief Get current Wi-Fi connection state
 * 
 * @return Current connection state
 */
net_wifi_state_t net_wifi_get_state(void);

/**
 * @brief Check if Wi-Fi is connected and has IP
 * 
 * @return true if connected, false otherwise
 */
bool net_wifi_is_connected(void);

/**
 * @brief Get Wi-Fi SSID if connected
 * 
 * @param[out] ssid Buffer to store SSID
 * @param[in] max_len Maximum length of ssid buffer
 * @return ESP_OK on success
 */
esp_err_t net_wifi_get_ssid(char *ssid, size_t max_len);

/**
 * @brief Connect to Wi-Fi network using stored credentials
 * 
 * Attempts to connect using credentials from NVS. If provisioning
 * is needed, will start provisioning automatically.
 * 
 * @return ESP_OK on success
 */
esp_err_t net_wifi_connect(void);

/**
 * @brief Disconnect from Wi-Fi network
 * 
 * @return ESP_OK on success
 */
esp_err_t net_wifi_disconnect(void);

/**
 * @brief Start Wi-Fi provisioning mode (SoftAP + Captive Portal)
 * 
 * Creates a SoftAP with SSID "P3A-Setup-XXXX" where XXXX is a random suffix.
 * Starts a captive portal on the SoftAP for credential provisioning.
 * 
 * @return ESP_OK on success
 */
esp_err_t net_wifi_start_provisioning(void);

/**
 * @brief Stop Wi-Fi provisioning mode
 * 
 * @return ESP_OK on success
 */
esp_err_t net_wifi_stop_provisioning(void);

#ifdef __cplusplus
}
#endif
