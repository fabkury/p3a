/**
 * @file ota_manager.h
 * @brief OTA (Over-The-Air) Update Manager for p3a
 * 
 * Provides functionality for checking, downloading, and installing
 * firmware updates from GitHub Releases.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTA state machine states
 */
typedef enum {
    OTA_STATE_IDLE,              ///< No update activity
    OTA_STATE_CHECKING,          ///< Querying GitHub API for updates
    OTA_STATE_UPDATE_AVAILABLE,  ///< New version found, awaiting user action
    OTA_STATE_DOWNLOADING,       ///< Downloading firmware
    OTA_STATE_VERIFYING,         ///< Verifying SHA256 checksum
    OTA_STATE_FLASHING,          ///< Writing to flash partition
    OTA_STATE_PENDING_REBOOT,    ///< Flash complete, reboot required
    OTA_STATE_ERROR,             ///< Error occurred
} ota_state_t;

/**
 * @brief OTA status information
 */
typedef struct {
    ota_state_t state;              ///< Current OTA state
    char current_version[32];       ///< Currently running firmware version
    char available_version[32];     ///< Available update version (if any)
    uint32_t available_size;        ///< Size of available update in bytes
    char release_notes[512];        ///< Release notes (truncated)
    int64_t last_check_time;        ///< Unix timestamp of last check
    int download_progress;          ///< Download progress 0-100
    char error_message[128];        ///< Error message if state is ERROR
    bool can_rollback;              ///< Whether rollback is available
    char rollback_version[32];      ///< Version to rollback to
    bool dev_mode;                  ///< Whether dev mode is enabled (pre-releases)
    bool is_prerelease;             ///< Whether available version is a pre-release
} ota_status_t;

/**
 * @brief Progress callback function type
 * 
 * @param percent Progress percentage (0-100)
 * @param status_text Current status message
 */
typedef void (*ota_progress_cb_t)(int percent, const char *status_text);

/**
 * @brief UI control callback function type
 * 
 * Called when OTA needs to control the display/animation system.
 * 
 * @param enter true when entering OTA mode (stop animations), false when exiting
 * @param version_from Current version (for display, may be NULL)
 * @param version_to Target version (for display, may be NULL)
 */
typedef void (*ota_ui_cb_t)(bool enter, const char *version_from, const char *version_to);

/**
 * @brief Initialize OTA manager
 * 
 * Starts periodic update check timer and initializes OTA subsystem.
 * Should be called after WiFi is connected.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ota_manager_init(void);

/**
 * @brief Deinitialize OTA manager
 * 
 * Stops timers and cleans up resources.
 */
void ota_manager_deinit(void);

/**
 * @brief Get current OTA state
 * 
 * @return Current OTA state
 */
ota_state_t ota_manager_get_state(void);

/**
 * @brief Get full OTA status
 * 
 * @param[out] status Pointer to status structure to fill
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL
 */
esp_err_t ota_manager_get_status(ota_status_t *status);

/**
 * @brief Trigger immediate update check
 * 
 * Non-blocking. Check runs in background task.
 * Use ota_manager_get_state() to poll for completion.
 * 
 * @return ESP_OK if check started, ESP_ERR_INVALID_STATE if check already in progress
 */
esp_err_t ota_manager_check_for_update(void);

/**
 * @brief Start firmware installation
 * 
 * Downloads and installs the available update.
 * Blocks until complete or error.
 * Device will reboot automatically on success.
 * 
 * @param progress_cb Optional callback for progress updates (can be NULL)
 * @param ui_cb Optional callback for UI control (can be NULL)
 * @return ESP_OK on success (device will reboot), error code otherwise
 */
esp_err_t ota_manager_install_update(ota_progress_cb_t progress_cb, ota_ui_cb_t ui_cb);

/**
 * @brief Schedule rollback to previous firmware and reboot
 * 
 * Sets boot partition to the other OTA slot and reboots.
 * 
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no rollback available
 */
esp_err_t ota_manager_rollback(void);

/**
 * @brief Validate current firmware after OTA update
 * 
 * Call this early in app_main() to confirm the new firmware is working.
 * If not called within boot timeout, automatic rollback will occur.
 * 
 * @return ESP_OK if validation succeeded or not needed, error otherwise
 */
esp_err_t ota_manager_validate_boot(void);

/**
 * @brief Check if OTA operations are currently blocked
 * 
 * OTA is blocked during PICO-8 streaming, USB MSC mode, etc.
 * 
 * @param[out] reason If not NULL, filled with reason string if blocked
 * @return true if OTA is blocked, false if allowed
 */
bool ota_manager_is_blocked(const char **reason);

/**
 * @brief Get string representation of OTA state
 * 
 * @param state OTA state
 * @return State name string
 */
const char *ota_state_to_string(ota_state_t state);

#ifdef __cplusplus
}
#endif

