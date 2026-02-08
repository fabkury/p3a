// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the show-url module
 *
 * Creates the persistent download task which sleeps until work is available.
 * Call once during boot, after SD card and WiFi are initialized.
 *
 * @return ESP_OK on success
 */
esp_err_t show_url_init(void);

/**
 * @brief Start downloading and displaying an artwork from a URL
 *
 * Cancels any in-flight download (show-url, download_manager, channel refresh)
 * and begins downloading the artwork to the animations directory.
 *
 * On success, the file is saved to animations-dir and playback starts
 * automatically via play_scheduler_play_local_file().
 *
 * @param artwork_url URL of the artwork (must end with .gif, .webp, .jpg, .jpeg, or .png)
 * @param blocking If true, display download progress on screen.
 *                 If false, download silently in the background.
 * @return ESP_OK if download was queued, error code otherwise
 */
esp_err_t show_url_start(const char *artwork_url, bool blocking);

/**
 * @brief Cancel the current show-url download (if any)
 *
 * Safe to call even if no download is in progress.
 */
void show_url_cancel(void);

/**
 * @brief Check if a show-url download is currently in progress
 *
 * @return true if downloading
 */
bool show_url_is_busy(void);

#ifdef __cplusplus
}
#endif
