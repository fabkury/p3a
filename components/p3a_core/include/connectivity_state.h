// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef CONNECTIVITY_STATE_H
#define CONNECTIVITY_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file connectivity_state.h
 * @brief Hierarchical connectivity state machine
 *
 * Provides a cascading view of network dependencies:
 *   NO_WIFI → NO_INTERNET → NO_REGISTRATION → NO_MQTT → ONLINE
 *
 * Each state implies satisfaction of all upstream states. For example,
 * NO_MQTT means WiFi is connected, internet is reachable, and the device
 * is registered with Makapix Club, but MQTT is not yet connected.
 *
 * ## User Messages
 *
 * | State           | Short            | Detail                                  |
 * |-----------------|------------------|-----------------------------------------|
 * | NO_WIFI         | "No Wi-Fi"       | "Connect to Wi-Fi network"              |
 * | NO_INTERNET     | "No Internet"    | "Wi-Fi connected but no internet access"|
 * | NO_REGISTRATION | "Not Registered" | "Long-press to register with Makapix"   |
 * | NO_MQTT         | "Connecting..."  | "Connecting to Makapix Cloud"           |
 * | ONLINE          | "Online"         | "Connected to Makapix Club"             |
 *
 * ## Implementation Details
 *
 * - **Internet check**: DNS lookup for `example.com` (no Makapix dependency)
 * - **Check frequency**: On WiFi connect, then every 60 seconds if NO_INTERNET
 * - **MQTT reconnection**: Exponential backoff with jitter (5s → 10s → ... → 300s max)
 */

/**
 * @brief Hierarchical connectivity states
 *
 * States are ordered by network stack depth. Each state implies
 * all upstream dependencies are satisfied.
 */
typedef enum {
    CONN_STATE_NO_WIFI = 0,       ///< WiFi not connected
    CONN_STATE_NO_INTERNET,       ///< WiFi connected, but no internet
    CONN_STATE_NO_REGISTRATION,   ///< Internet available, but no Makapix registration
    CONN_STATE_NO_MQTT,           ///< Registered, but MQTT not connected
    CONN_STATE_ONLINE,            ///< Fully connected to Makapix Cloud
} connectivity_state_t;

/**
 * @brief Callback type for connectivity state changes
 *
 * @param old_state Previous state
 * @param new_state New state
 * @param user_ctx User context passed to register
 */
typedef void (*connectivity_state_cb_t)(connectivity_state_t old_state,
                                         connectivity_state_t new_state,
                                         void *user_ctx);

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize the connectivity state subsystem
 *
 * Registers for WiFi/MQTT events and starts the state machine.
 * Initial state is NO_WIFI.
 *
 * @return ESP_OK on success
 */
esp_err_t connectivity_state_init(void);

/**
 * @brief Deinitialize the connectivity state subsystem
 */
void connectivity_state_deinit(void);

// ============================================================================
// State Access
// ============================================================================

/**
 * @brief Get current connectivity state
 *
 * @return Current state
 */
connectivity_state_t connectivity_state_get(void);

/**
 * @brief Get short message for current state
 *
 * Returns a brief user-facing message like "No Wi-Fi" or "Online".
 *
 * @return Static string (do not free)
 */
const char *connectivity_state_get_message(void);

/**
 * @brief Get detailed message for current state
 *
 * Returns a longer message explaining the current state.
 *
 * @return Static string (do not free)
 */
const char *connectivity_state_get_detail(void);

/**
 * @brief Check if fully online
 *
 * @return true if state is CONN_STATE_ONLINE
 */
bool connectivity_state_is_online(void);

/**
 * @brief Check if internet is available
 *
 * Returns true for NO_REGISTRATION, NO_MQTT, and ONLINE states.
 *
 * @return true if internet is reachable
 */
bool connectivity_state_has_internet(void);

/**
 * @brief Check if WiFi is connected
 *
 * Returns true for all states except NO_WIFI.
 *
 * @return true if WiFi is connected
 */
bool connectivity_state_has_wifi(void);

// ============================================================================
// Waiting
// ============================================================================

/**
 * @brief Wait for connectivity to reach ONLINE state
 *
 * Blocks until the device is fully connected or timeout expires.
 *
 * @param timeout_ms Maximum wait time in milliseconds (portMAX_DELAY for infinite)
 * @return ESP_OK if online, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t connectivity_state_wait_for_online(TickType_t timeout_ms);

/**
 * @brief Wait for internet connectivity
 *
 * Blocks until internet is reachable (state >= NO_REGISTRATION) or timeout.
 *
 * @param timeout_ms Maximum wait time in milliseconds
 * @return ESP_OK if internet available, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t connectivity_state_wait_for_internet(TickType_t timeout_ms);

/**
 * @brief Wait for WiFi connection
 *
 * Blocks until WiFi is connected (state >= NO_INTERNET) or timeout.
 *
 * @param timeout_ms Maximum wait time in milliseconds
 * @return ESP_OK if WiFi connected, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t connectivity_state_wait_for_wifi(TickType_t timeout_ms);

// ============================================================================
// Callbacks
// ============================================================================

/**
 * @brief Register callback for state changes
 *
 * Callbacks are invoked synchronously when state changes.
 * Up to 8 callbacks can be registered.
 *
 * @param cb Callback function
 * @param user_ctx User context passed to callback
 * @return ESP_OK on success, ESP_ERR_NO_MEM if too many callbacks
 */
esp_err_t connectivity_state_register_callback(connectivity_state_cb_t cb, void *user_ctx);

/**
 * @brief Unregister a callback
 *
 * @param cb Callback to unregister
 */
void connectivity_state_unregister_callback(connectivity_state_cb_t cb);

// ============================================================================
// Event Handlers (called by WiFi/MQTT components)
// ============================================================================

/**
 * @brief Notify that WiFi connected
 *
 * Called by wifi_manager when WiFi connects. Transitions to NO_INTERNET
 * and triggers internet check.
 */
void connectivity_state_on_wifi_connected(void);

/**
 * @brief Notify that WiFi disconnected
 *
 * Called by wifi_manager when WiFi disconnects. Transitions to NO_WIFI.
 */
void connectivity_state_on_wifi_disconnected(void);

/**
 * @brief Notify that MQTT connected
 *
 * Called by makapix_mqtt when MQTT connects. Transitions to ONLINE.
 */
void connectivity_state_on_mqtt_connected(void);

/**
 * @brief Notify that MQTT disconnected
 *
 * Called by makapix_mqtt when MQTT disconnects. Transitions to NO_MQTT
 * (if still registered) or appropriate earlier state.
 */
void connectivity_state_on_mqtt_disconnected(void);

/**
 * @brief Notify that registration status changed
 *
 * Called when Makapix registration is completed or invalidated.
 *
 * @param has_registration true if device is registered
 */
void connectivity_state_on_registration_changed(bool has_registration);

// ============================================================================
// Internet Check
// ============================================================================

/**
 * @brief Force an internet connectivity check
 *
 * Performs a DNS lookup for example.com to verify internet access.
 * Updates state based on result.
 *
 * @return true if internet is reachable
 */
bool connectivity_state_check_internet(void);

/**
 * @brief Get time since last successful internet check
 *
 * @return Seconds since last successful check, or UINT32_MAX if never
 */
uint32_t connectivity_state_get_last_internet_check_age(void);

#ifdef __cplusplus
}
#endif

#endif // CONNECTIVITY_STATE_H
