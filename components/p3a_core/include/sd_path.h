// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Default SD card root folder for p3a data
 * 
 * All p3a data is stored under this folder on the SD card.
 * This is the full path including the SD mount point (/sdcard).
 * Users configure a user-friendly path (e.g., /p3a) which gets
 * prepended with /sdcard internally.
 * 
 * This can be configured via the web UI, but requires a reboot to take effect.
 */
#define SD_PATH_DEFAULT_ROOT "/sdcard/p3a"

/**
 * @brief Maximum length of the root path
 */
#define SD_PATH_ROOT_MAX_LEN 64

/**
 * @brief Initialize the SD path module
 * 
 * Loads the configured root path from NVS. If not set, uses the default.
 * This should be called once during startup, before any SD card operations.
 * 
 * @return ESP_OK on success
 */
esp_err_t sd_path_init(void);

/**
 * @brief Get the SD card root folder for p3a
 * 
 * @return Pointer to the root path string (e.g., "/sdcard/p3a")
 */
const char *sd_path_get_root(void);

/**
 * @brief Build a full path for a subdirectory under the p3a root
 * 
 * @param subdir Subdirectory name (e.g., "animations", "vault", "channel")
 * @param out_path Output buffer for the full path
 * @param out_len Size of output buffer
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if buffer too small
 */
esp_err_t sd_path_get_subdir(const char *subdir, char *out_path, size_t out_len);

/**
 * @brief Get the animations directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_animations(char *out_path, size_t out_len);

/**
 * @brief Get the vault directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_vault(char *out_path, size_t out_len);

/**
 * @brief Get the channel directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_channel(char *out_path, size_t out_len);

/**
 * @brief Get the playlists directory path
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_playlists(char *out_path, size_t out_len);

/**
 * @brief Get the downloads directory path (for temporary uploads)
 * 
 * @param out_path Output buffer for the path
 * @param out_len Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t sd_path_get_downloads(char *out_path, size_t out_len);

/**
 * @brief Set the SD card root folder (persisted to NVS, requires reboot)
 * 
 * Accepts either:
 * - User-friendly path: "/p3a", "/data", "/myproject" (recommended)
 * - Full path: "/sdcard/p3a", "/sdcard/data" (for compatibility)
 * 
 * The user-friendly format is stored in NVS, and /sdcard is prepended
 * internally at runtime.
 * 
 * @param root_path New root path (e.g., "/p3a" or "/sdcard/p3a")
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if validation fails
 */
esp_err_t sd_path_set_root(const char *root_path);

/**
 * @brief Create all required subdirectories under the p3a root
 * 
 * Creates: animations, vault, channel, playlists, downloads
 * 
 * @return ESP_OK on success, or error if directory creation fails
 */
esp_err_t sd_path_ensure_directories(void);

#ifdef __cplusplus
}
#endif

