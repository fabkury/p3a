// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_state.h
 * @brief Unified p3a Global State Machine
 * 
 * This module provides the central state machine for p3a, managing:
 * - Global application states (animation playback, provisioning, OTA, PICO-8)
 * - State entry rules and transitions
 * - Per-state touch handling routing
 * - Per-state graphics rendering
 * - Channel and state persistence
 * 
 * ARCHITECTURE OVERVIEW:
 * ----------------------
 * p3a operates as a unified state machine where each state has:
 * 1. Entry rules - conditions that must be met to enter the state
 * 2. Touch handler - state-specific gesture processing
 * 3. Render function - state-specific graphics output
 * 4. Exit cleanup - actions to perform when leaving the state
 * 
 * GLOBAL STATES:
 * --------------
 * - ANIMATION_PLAYBACK: Normal operation, displaying animations from channels
 *   Sub-states: playing, channel_message (download status, empty channel, etc.)
 * 
 * - PROVISIONING: Makapix device registration flow
 *   Sub-states: status, show_code, captive_ap_info
 * 
 * - OTA: Firmware update in progress
 *   Sub-states: checking, downloading, verifying, flashing
 * 
 * - PICO8_STREAMING: Real-time PICO-8 frame streaming from USB/WiFi
 * 
 * BOOT BEHAVIOR:
 * --------------
 * On boot, p3a:
 * 1. Loads remembered channel from NVS (defaults to sdcard-channel if none)
 * 2. Enters ANIMATION_PLAYBACK state with that channel
 * 3. Other states remembered for utility but not restored on boot
 */

#ifndef P3A_STATE_H
#define P3A_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// STATE DEFINITIONS
// ============================================================================

/**
 * @brief Global p3a states
 */
typedef enum {
    P3A_STATE_BOOT,                 ///< Boot sequence (initializing subsystems)
    P3A_STATE_ANIMATION_PLAYBACK,   ///< Normal animation playback from channels
    P3A_STATE_PROVISIONING,         ///< Makapix device registration
    P3A_STATE_OTA,                  ///< Firmware update in progress
    P3A_STATE_PICO8_STREAMING,      ///< Real-time PICO-8 streaming
    P3A_STATE_ERROR,                ///< Critical error state
} p3a_state_t;

/**
 * @brief Connectivity level (orthogonal to global state)
 */
typedef enum {
    P3A_CONNECTIVITY_NO_WIFI = 0,   ///< WiFi not connected
    P3A_CONNECTIVITY_NO_INTERNET,   ///< WiFi connected, but no internet
    P3A_CONNECTIVITY_NO_REGISTRATION, ///< Internet available, no Makapix registration
    P3A_CONNECTIVITY_NO_MQTT,       ///< Registered, but MQTT not connected
    P3A_CONNECTIVITY_ONLINE,        ///< Fully connected to Makapix Cloud
} p3a_connectivity_level_t;

/**
 * @brief Animation playback sub-states
 */
typedef enum {
    P3A_PLAYBACK_PLAYING,           ///< Normal animation display
    P3A_PLAYBACK_CHANNEL_MESSAGE,   ///< Displaying channel status message
} p3a_playback_substate_t;

/**
 * @brief Channel message types for CHANNEL_MESSAGE sub-state
 */
typedef enum {
    P3A_CHANNEL_MSG_NONE,           ///< No message
    P3A_CHANNEL_MSG_FETCHING,       ///< "Fetching artwork"
    P3A_CHANNEL_MSG_DOWNLOADING,    ///< "Downloading artwork: X%"
    P3A_CHANNEL_MSG_DOWNLOAD_FAILED,///< "Download failed, retrying"
    P3A_CHANNEL_MSG_EMPTY,          ///< "Channel empty"
    P3A_CHANNEL_MSG_LOADING,        ///< "Loading channel..."
    P3A_CHANNEL_MSG_ERROR,          ///< "Failed to load channel"
} p3a_channel_msg_type_t;

/**
 * @brief Provisioning sub-states
 */
typedef enum {
    P3A_PROV_STATUS,                ///< Showing status message
    P3A_PROV_SHOW_CODE,             ///< Showing registration code
    P3A_PROV_CAPTIVE_AP_INFO,       ///< Showing WiFi setup instructions
} p3a_provisioning_substate_t;

/**
 * @brief OTA sub-states
 */
typedef enum {
    P3A_OTA_CHECKING,               ///< Checking for updates
    P3A_OTA_DOWNLOADING,            ///< Downloading firmware
    P3A_OTA_VERIFYING,              ///< Verifying checksum
    P3A_OTA_FLASHING,               ///< Writing to flash
    P3A_OTA_PENDING_REBOOT,         ///< Waiting for reboot
} p3a_ota_substate_t;

/**
 * @brief Application-level status (legacy app_state replacement)
 */
typedef enum {
    P3A_APP_STATUS_READY = 0,       ///< Normal operation/idle state
    P3A_APP_STATUS_PROCESSING,      ///< Executing a command
    P3A_APP_STATUS_ERROR            ///< Unrecoverable error state
} p3a_app_status_t;

/**
 * @brief Channel types
 */
typedef enum {
    P3A_CHANNEL_SDCARD,             ///< Local SD card channel
    P3A_CHANNEL_MAKAPIX_ALL,        ///< Makapix "all" channel
    P3A_CHANNEL_MAKAPIX_PROMOTED,   ///< Makapix "promoted" channel
    P3A_CHANNEL_MAKAPIX_USER,       ///< Makapix "user" (following) channel
    P3A_CHANNEL_MAKAPIX_BY_USER,    ///< Makapix "by_user" channel (specific artist)
    P3A_CHANNEL_MAKAPIX_HASHTAG,    ///< Makapix "hashtag" channel (specific hashtag)
    P3A_CHANNEL_MAKAPIX_ARTWORK,    ///< Transient single-artwork channel
} p3a_channel_type_t;

/**
 * @brief Current channel information
 */
typedef struct {
    p3a_channel_type_t type;
    char identifier[64];            ///< For BY_USER (user_sqid) or HASHTAG (hashtag) channels
    char storage_key[64];           ///< For ARTWORK channel
    char display_name[64];          ///< Human-readable channel name
} p3a_channel_info_t;

/**
 * @brief Channel message information
 */
typedef struct {
    p3a_channel_msg_type_t type;
    char channel_name[64];          ///< Channel being loaded
    int progress_percent;           ///< Download progress (0-100, -1 if unknown)
    char detail[128];               ///< Additional detail text
} p3a_channel_message_t;

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize the p3a state machine
 * 
 * Loads persisted channel from NVS and enters ANIMATION_PLAYBACK state.
 * Must be called before any other p3a_state functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t p3a_state_init(void);

/**
 * @brief Deinitialize the state machine
 */
void p3a_state_deinit(void);

// ============================================================================
// STATE QUERIES
// ============================================================================

/**
 * @brief Get current global state
 */
p3a_state_t p3a_state_get(void);

/**
 * @brief Get state name string
 */
const char *p3a_state_get_name(p3a_state_t state);

/**
 * @brief Get current application status (READY/PROCESSING/ERROR)
 */
p3a_app_status_t p3a_state_get_app_status(void);

/**
 * @brief Get string representation of application status
 */
const char *p3a_state_get_app_status_name(p3a_app_status_t status);

/**
 * @brief Get current connectivity level
 */
p3a_connectivity_level_t p3a_state_get_connectivity(void);

/**
 * @brief Get short connectivity message
 */
const char *p3a_state_get_connectivity_message(void);

/**
 * @brief Get detailed connectivity message
 */
const char *p3a_state_get_connectivity_detail(void);

/**
 * @brief Check if WiFi is connected
 */
bool p3a_state_has_wifi(void);

/**
 * @brief Check if internet is reachable
 */
bool p3a_state_has_internet(void);

/**
 * @brief Check if fully online
 */
bool p3a_state_is_online(void);

/**
 * @brief Get current playback sub-state
 * 
 * Only valid when global state is P3A_STATE_ANIMATION_PLAYBACK.
 */
p3a_playback_substate_t p3a_state_get_playback_substate(void);

/**
 * @brief Get current provisioning sub-state
 * 
 * Only valid when global state is P3A_STATE_PROVISIONING.
 */
p3a_provisioning_substate_t p3a_state_get_provisioning_substate(void);

/**
 * @brief Get current OTA sub-state
 * 
 * Only valid when global state is P3A_STATE_OTA.
 */
p3a_ota_substate_t p3a_state_get_ota_substate(void);

/**
 * @brief Get current channel information
 */
esp_err_t p3a_state_get_channel_info(p3a_channel_info_t *out_info);

/**
 * @brief Get current channel message (if in CHANNEL_MESSAGE sub-state)
 */
esp_err_t p3a_state_get_channel_message(p3a_channel_message_t *out_msg);

// ============================================================================
// STATE TRANSITIONS
// ============================================================================

/**
 * @brief Request transition to ANIMATION_PLAYBACK state
 * 
 * Entry rules:
 * - Can be entered from any state
 * 
 * @return ESP_OK if transition successful, error if denied
 */
esp_err_t p3a_state_enter_animation_playback(void);

/**
 * @brief Request transition to PROVISIONING state
 * 
 * Entry rules:
 * - Can be entered from ANIMATION_PLAYBACK
 * - Cannot enter during OTA
 * 
 * @return ESP_OK if transition successful, ESP_ERR_INVALID_STATE if denied
 */
esp_err_t p3a_state_enter_provisioning(void);

/**
 * @brief Request transition to OTA state
 * 
 * Entry rules:
 * - Can be entered from ANIMATION_PLAYBACK
 * - Cannot enter during PROVISIONING or PICO8_STREAMING
 * 
 * @return ESP_OK if transition successful, ESP_ERR_INVALID_STATE if denied
 */
esp_err_t p3a_state_enter_ota(void);

/**
 * @brief Request transition to PICO8_STREAMING state
 * 
 * Entry rules:
 * - Can only be entered from ANIMATION_PLAYBACK
 * 
 * @return ESP_OK if transition successful, ESP_ERR_INVALID_STATE if denied
 */
esp_err_t p3a_state_enter_pico8_streaming(void);

/**
 * @brief Exit current state and return to ANIMATION_PLAYBACK
 * 
 * This is a convenience function for states that need to "go back" to normal.
 */
esp_err_t p3a_state_exit_to_playback(void);

/**
 * @brief Enter error state
 */
esp_err_t p3a_state_enter_error(void);

// ============================================================================
// SUB-STATE UPDATES
// ============================================================================

/**
 * @brief Set playback sub-state to PLAYING
 */
void p3a_state_set_playback_playing(void);

/**
 * @brief Set playback sub-state to CHANNEL_MESSAGE with details
 */
void p3a_state_set_channel_message(const p3a_channel_message_t *msg);

/**
 * @brief Set provisioning sub-state
 */
void p3a_state_set_provisioning_substate(p3a_provisioning_substate_t substate);

/**
 * @brief Set OTA sub-state
 */
void p3a_state_set_ota_substate(p3a_ota_substate_t substate);

/**
 * @brief Update OTA progress
 */
void p3a_state_set_ota_progress(int percent, const char *status_text);

// ============================================================================
// APP STATUS (legacy app_state replacement)
// ============================================================================

/**
 * @brief Set application status to READY
 */
void p3a_state_enter_ready(void);

/**
 * @brief Set application status to PROCESSING
 */
void p3a_state_enter_processing(void);

/**
 * @brief Set application status to ERROR
 */
void p3a_state_enter_app_error(void);

/**
 * @brief Set application status explicitly
 */
void p3a_state_set_app_status(p3a_app_status_t status);

// ============================================================================
// CONNECTIVITY (orthogonal state)
// ============================================================================

/**
 * @brief Initialize connectivity tracking (internal use)
 */
esp_err_t p3a_state_connectivity_init(void);

/**
 * @brief Deinitialize connectivity tracking (internal use)
 */
void p3a_state_connectivity_deinit(void);

/**
 * @brief Event handlers called by WiFi/MQTT components
 */
void p3a_state_on_wifi_connected(void);
void p3a_state_on_wifi_disconnected(void);
void p3a_state_on_mqtt_connected(void);
void p3a_state_on_mqtt_disconnected(void);
void p3a_state_on_registration_changed(bool has_registration);

/**
 * @brief Force an internet connectivity check
 */
bool p3a_state_check_internet(void);

/**
 * @brief Get time since last successful internet check (seconds)
 */
uint32_t p3a_state_get_last_internet_check_age(void);

/**
 * @brief Wait for connectivity to reach ONLINE state
 */
esp_err_t p3a_state_wait_for_online(TickType_t timeout_ms);

/**
 * @brief Wait for internet connectivity
 */
esp_err_t p3a_state_wait_for_internet(TickType_t timeout_ms);

/**
 * @brief Wait for WiFi connection
 */
esp_err_t p3a_state_wait_for_wifi(TickType_t timeout_ms);

// ============================================================================
// CHANNEL MANAGEMENT
// ============================================================================

/**
 * @brief Switch to a channel
 * 
 * Performs a "cross-channel swap" - the next artwork displayed comes from
 * the new channel. If the channel has no artworks available, enters
 * CHANNEL_MESSAGE sub-state with appropriate message.
 * 
 * @param type Channel type
 * @param user_handle User handle (required for BY_USER, ignored otherwise)
 * @return ESP_OK on success
 */
esp_err_t p3a_state_switch_channel(p3a_channel_type_t type, const char *identifier);

/**
 * @brief Switch to single-artwork channel (for show_artwork command)
 * 
 * Creates a transient in-memory channel with one artwork.
 * Handles download with progress display if artwork not cached.
 * 
 * @param storage_key Artwork storage key UUID
 * @param art_url Artwork download URL
 * @param post_id Post ID for view tracking
 * @return ESP_OK on success
 */
esp_err_t p3a_state_show_artwork(const char *storage_key, const char *art_url, int32_t post_id);

/**
 * @brief Fall back to SD card channel
 * 
 * Convenience function equivalent to p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL).
 * Used when artwork download fails after retries.
 */
esp_err_t p3a_state_fallback_to_sdcard(void);

/**
 * @brief Get the default channel (sdcard or last remembered)
 */
p3a_channel_type_t p3a_state_get_default_channel(void);

// ============================================================================
// PERSISTENCE
// ============================================================================

/**
 * @brief Maximum length for playset names
 */
#define P3A_PLAYSET_MAX_NAME_LEN 32

/**
 * @brief Set and persist the active playset name to NVS
 *
 * This is the primary persistence mechanism for playback state.
 * Built-in playset names: "channel_recent", "channel_promoted", "channel_sdcard"
 * Server playsets: "followed_artists", etc.
 *
 * @param name Playset name (max P3A_PLAYSET_MAX_NAME_LEN chars)
 * @return ESP_OK on success
 */
esp_err_t p3a_state_set_active_playset(const char *name);

/**
 * @brief Get the currently active playset name
 *
 * @return Playset name or empty string if none set
 */
const char *p3a_state_get_active_playset(void);

/**
 * @brief Save current channel to NVS
 *
 * Called automatically when channel changes.
 *
 * @deprecated Use p3a_state_set_active_playset() instead
 */
esp_err_t p3a_state_persist_channel(void);

/**
 * @brief Load channel from NVS
 *
 * Called during init. Returns P3A_CHANNEL_SDCARD if no saved channel.
 *
 * @deprecated Channel persistence is being replaced by playset persistence
 */
esp_err_t p3a_state_load_channel(p3a_channel_info_t *out_info);

// ============================================================================
// CALLBACKS (for integration with other components)
// ============================================================================

/**
 * @brief State change callback type
 */
typedef void (*p3a_state_change_cb_t)(p3a_state_t old_state, p3a_state_t new_state, void *user_data);

/**
 * @brief Register state change callback
 */
esp_err_t p3a_state_register_callback(p3a_state_change_cb_t callback, void *user_data);

/**
 * @brief Unregister state change callback
 */
void p3a_state_unregister_callback(p3a_state_change_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif // P3A_STATE_H

