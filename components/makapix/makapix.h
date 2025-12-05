#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Makapix state enumeration
 */
typedef enum {
    MAKAPIX_STATE_IDLE,           // No player_key, waiting for provision gesture
    MAKAPIX_STATE_PROVISIONING,   // HTTP request in progress
    MAKAPIX_STATE_SHOW_CODE,      // Displaying registration code
    MAKAPIX_STATE_CONNECTING,     // MQTT connecting
    MAKAPIX_STATE_CONNECTED,      // Normal operation
    MAKAPIX_STATE_DISCONNECTED    // MQTT lost, reconnecting
} makapix_state_t;

/**
 * @brief Initialize Makapix module
 * 
 * Loads stored credentials from NVS and sets up initial state.
 * Must be called before any other makapix functions.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_init(void);

/**
 * @brief Get current Makapix state
 * 
 * @return Current state
 */
makapix_state_t makapix_get_state(void);

/**
 * @brief Start provisioning process
 * 
 * Called when user triggers 10-second long press gesture.
 * Initiates HTTP request to provisioning endpoint.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t makapix_start_provisioning(void);

/**
 * @brief Cancel provisioning process
 * 
 * Cancels any ongoing provisioning and returns to idle state.
 */
void makapix_cancel_provisioning(void);

/**
 * @brief Get current post ID being displayed
 * 
 * Returns 0 when playing local animations from /sdcard/animations/.
 * Will return actual post_id when playing from /sdcard/vault/ (future).
 * 
 * @return Current post ID, or 0 for local animations
 */
int32_t makapix_get_current_post_id(void);

/**
 * @brief Set current post ID
 * 
 * Called when artwork changes to update the tracked post ID.
 * 
 * @param post_id Post ID to set (0 for local animations)
 */
void makapix_set_current_post_id(int32_t post_id);

/**
 * @brief Connect to MQTT if credentials are available
 * 
 * Should be called after WiFi is connected.
 * 
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no credentials, error code otherwise
 */
esp_err_t makapix_connect_if_registered(void);

/**
 * @brief Get registration code for display
 * 
 * @param out_code Buffer to receive code (must be at least 7 bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK if code available, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t makapix_get_registration_code(char *out_code, size_t max_len);

/**
 * @brief Get registration code expiration time
 * 
 * @param out_expires Buffer to receive ISO 8601 timestamp (must be at least 32 bytes)
 * @param max_len Maximum length of buffer
 * @return ESP_OK if available, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t makapix_get_registration_expires(char *out_expires, size_t max_len);

/**
 * @brief Set provisioning status message
 * 
 * Updates the status message shown during provisioning.
 * Only works when in MAKAPIX_STATE_PROVISIONING state.
 * 
 * @param status_message Status message to display
 */
void makapix_set_provisioning_status(const char *status_message);

/**
 * @brief Get current provisioning status message
 * 
 * @param out_status Buffer to receive status message
 * @param max_len Maximum length of buffer
 * @return ESP_OK if available, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t makapix_get_provisioning_status(char *out_status, size_t max_len);

