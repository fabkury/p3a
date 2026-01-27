// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

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
 * @brief Web UI OTA state machine states
 */
typedef enum {
    WEBUI_OTA_STATE_IDLE,        ///< No web UI update activity
    WEBUI_OTA_STATE_DOWNLOADING, ///< Downloading storage.bin
    WEBUI_OTA_STATE_UNMOUNTING,  ///< Unmounting LittleFS
    WEBUI_OTA_STATE_ERASING,     ///< Erasing storage partition
    WEBUI_OTA_STATE_WRITING,     ///< Writing to storage partition
    WEBUI_OTA_STATE_VERIFYING,   ///< Verifying written data
    WEBUI_OTA_STATE_REMOUNTING,  ///< Remounting LittleFS
    WEBUI_OTA_STATE_COMPLETE,    ///< Update complete
    WEBUI_OTA_STATE_ERROR,       ///< Error occurred
} webui_ota_state_t;

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
 * @brief Web UI OTA status information
 */
typedef struct {
    char current_version[16];       ///< Current web UI version (X.Y format)
    char available_version[16];     ///< Available web UI version (if any)
    bool update_available;          ///< True if a newer version is available
    bool partition_valid;           ///< True if storage partition is valid
    bool needs_recovery;            ///< True if auto-recovery is pending
    bool auto_update_disabled;      ///< True if too many failures disabled auto-update
    uint8_t failure_count;          ///< Consecutive OTA failure count
    webui_ota_state_t state;        ///< Current web UI OTA state
    int progress;                   ///< Progress percentage (0-100)
    char status_message[64];        ///< Human-readable status message
    char error_message[128];        ///< Error message if state is ERROR
} webui_ota_status_t;

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
 * @brief Check if OTA is currently checking for updates
 * 
 * Used by animation player to avoid SDIO bus contention.
 * Animation swaps should be deferred when this returns true.
 * 
 * @return true if OTA check is in progress, false otherwise
 */
bool ota_manager_is_checking(void);

/**
 * @brief Get string representation of OTA state
 *
 * @param state OTA state
 * @return State name string
 */
const char *ota_state_to_string(ota_state_t state);

// =============================================================================
// Web UI OTA Functions (storage partition updates)
// =============================================================================

/**
 * @brief Get string representation of web UI OTA state
 *
 * @param state Web UI OTA state
 * @return State name string
 */
const char *webui_ota_state_to_string(webui_ota_state_t state);

/**
 * @brief Get current web UI version
 *
 * Reads version from /spiffs/version.txt
 *
 * @param[out] version Buffer to store version string (at least 16 bytes)
 * @param buf_size Size of version buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if version.txt missing
 */
esp_err_t webui_ota_get_current_version(char *version, size_t buf_size);

/**
 * @brief Get web UI OTA status
 *
 * @param[out] status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t webui_ota_get_status(webui_ota_status_t *status);

/**
 * @brief Check if web UI partition is healthy
 *
 * Checks NVS flag and verifies version.txt exists.
 *
 * @return true if partition is healthy, false if recovery needed
 */
bool webui_ota_is_partition_healthy(void);

/**
 * @brief Set storage partition needs recovery flag
 *
 * Called when partition corruption is detected.
 */
void webui_ota_set_needs_recovery(void);

/**
 * @brief Trigger web UI repair (force re-download)
 *
 * Forces a re-download of storage.bin regardless of version.
 * Used for manual recovery when web UI is broken.
 *
 * @return ESP_OK if repair started, error code otherwise
 */
esp_err_t webui_ota_trigger_repair(void);

/**
 * @brief Install web UI update
 *
 * Downloads and writes storage.bin to the LittleFS partition.
 * This function handles:
 * - SHA256 verification of download
 * - Defensive NVS flag management
 * - Partition erase and write
 * - Post-write verification
 * - LittleFS remount
 *
 * @param download_url URL to download storage.bin from
 * @param expected_sha256 Expected SHA256 hash (64-char hex string)
 * @param progress_cb Optional progress callback
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t webui_ota_install_update(const char *download_url,
                                    const char *expected_sha256,
                                    ota_progress_cb_t progress_cb);

#ifdef __cplusplus
}
#endif

