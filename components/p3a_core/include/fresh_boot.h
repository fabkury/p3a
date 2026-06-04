// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file fresh_boot.h
 * @brief Fresh boot interface for debugging first-run behavior
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

/**
 * @brief Erase p3a-specific NVS namespaces to simulate fresh boot
 * 
 * This function erases the following NVS namespaces:
 * - "p3a_boot" (firmware version tracking)
 * - "appcfg" (app configuration/settings)
 * - "p3a_state" (channel state)
 * - "makapix" (Makapix credentials)
 * - "wifi_config" (only if CONFIG_P3A_FORCE_FRESH_WIFI is enabled)
 * 
 * This is intended for debugging fresh device behavior.
 * Should be called after nvs_flash_init() in app_main().
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fresh_boot_erase_nvs(void);

/**
 * @brief Delete and recreate the p3a SD card root directory
 *
 * This function:
 * 1. Recursively deletes the configured SD root (default /sdcard/p3a,
 *    resolved via sd_path_get_root()) and all its contents
 * 2. Recreates the empty root directory
 *
 * This is intended for debugging fresh device behavior.
 * Should be called after the SD card is mounted and NVS is initialized
 * (sd_path lazy-initializes from NVS to resolve the configured root).
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fresh_boot_erase_sdcard(void);

#ifdef __cplusplus
}
#endif

