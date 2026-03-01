// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check available space and run eviction if needed
 *
 * Checks SNTP synchronization, then queries free space on /sdcard.
 * If free space is below the configured target, runs multi-pass
 * age-based eviction over the vault and giphy cache directories.
 *
 * Each pass halves the age threshold, starting from
 * STORAGE_EVICTION_INITIAL_AGE_DAYS and stopping at
 * STORAGE_EVICTION_MIN_AGE_HOURS.
 *
 * Safe to call from any context; fast-returns if space is sufficient.
 *
 * @return ESP_OK on success (eviction ran or not needed)
 * @return ESP_ERR_INVALID_STATE if SNTP is not synchronized
 * @return ESP_FAIL on filesystem error
 */
esp_err_t storage_eviction_check_and_run(void);

/**
 * @brief Get free space on /sdcard
 *
 * Convenience wrapper around statvfs("/sdcard").
 *
 * @param[out] out_free_bytes Receives the number of free bytes
 * @return ESP_OK on success, ESP_FAIL on statvfs error
 */
esp_err_t storage_eviction_get_free_space(uint64_t *out_free_bytes);

/**
 * @brief Get total and free space on /sdcard
 *
 * @param[out] out_total_bytes Receives the total size in bytes (NULL to skip)
 * @param[out] out_free_bytes  Receives the free space in bytes (NULL to skip)
 * @return ESP_OK on success, ESP_FAIL on filesystem error
 */
esp_err_t storage_eviction_get_storage_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

/**
 * @brief Evict stale channel files from the channel directory
 *
 * Scans /sdcard/p3a/channel/ for .cache files whose mtime is older than
 * CONFIG_CHANNEL_EVICTION_AGE_DAYS. For each stale channel, deletes the
 * .cache, .json, .settings.json, and .bin files. Channels in the active
 * playset and the SD card channel are always protected.
 *
 * Does not delete artwork files (vault/giphy) -- those are handled
 * separately by storage_eviction_check_and_run().
 *
 * Safe to call from any context; fast-returns if SNTP is not synced.
 *
 * @return ESP_OK on success (eviction ran or nothing to evict)
 * @return ESP_ERR_INVALID_STATE if SNTP is not synchronized
 * @return ESP_FAIL on filesystem error
 */
esp_err_t channel_eviction_check_and_run(void);

#ifdef __cplusplus
}
#endif
