// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load configuration from NVS
 * 
 * Reads the current config from NVS namespace "appcfg" and returns
 * a deep-copied cJSON object. Caller must free with cJSON_Delete().
 * 
 * If config is missing or corrupt, returns an empty JSON object {}.
 * 
 * @param out_cfg Pointer to receive the cJSON object (must be freed by caller)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_load(cJSON **out_cfg);

/**
 * @brief Save configuration to NVS atomically
 * 
 * Validates that the config is a JSON object and serialized size <= 32 KB.
 * Saves atomically: writes to temp key, validates, then swaps to main key.
 * 
 * @param cfg cJSON object to save (must be a JSON object)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_save(const cJSON *cfg);

/**
 * @brief Get current config as serialized JSON string
 * 
 * Returns a malloc'd string containing the current config JSON.
 * Caller must free the returned string.
 * 
 * @param out_json Pointer to receive the JSON string (must be freed by caller)
 * @param out_len Pointer to receive the string length (excluding null terminator)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_get_serialized(char **out_json, size_t *out_len);

/**
 * @brief Set screen rotation in config
 * 
 * @param rotation Rotation angle (0°, 90°, 180°, 270°)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_store_set_rotation(uint16_t rotation_degrees);

/**
 * @brief Get screen rotation from config
 * 
 * @return Current rotation angle (defaults to ROTATION_0 if not set)
 */
uint16_t config_store_get_rotation(void);

/**
 * @brief Set play order mode
 * 
 * @param order Play order (0=server, 1=created, 2=random)
 * @return ESP_OK on success
 */
esp_err_t config_store_set_play_order(uint8_t order);

/**
 * @brief Get play order mode
 * 
 * @return Play order (defaults to 0/server if not set)
 */
uint8_t config_store_get_play_order(void);

/**
 * @brief Set randomize playlist mode
 * 
 * @param enable True to randomize playlists internally
 * @return ESP_OK on success
 */
esp_err_t config_store_set_randomize_playlist(bool enable);

/**
 * @brief Get randomize playlist mode
 * 
 * @return True if randomize playlist enabled (defaults to false)
 */
bool config_store_get_randomize_playlist(void);

/**
 * @brief Set Live Mode
 * 
 * @param enable True to enable Live Mode sync
 * @return ESP_OK on success
 */
esp_err_t config_store_set_live_mode(bool enable);

/**
 * @brief Get Live Mode
 * 
 * @return True if Live Mode enabled (defaults to false)
 */
bool config_store_get_live_mode(void);

/**
 * @brief Set dwell time
 * 
 * @param dwell_time_ms Dwell time in milliseconds
 * @return ESP_OK on success
 */
esp_err_t config_store_set_dwell_time(uint32_t dwell_time_ms);

/**
 * @brief Get dwell time
 * 
 * @return Dwell time in milliseconds (defaults to 30000)
 */
uint32_t config_store_get_dwell_time(void);

/**
 * @brief Set global random seed (persisted, applied after reboot)
 *
 * @param seed Global seed (default 0xFAB)
 */
esp_err_t config_store_set_global_seed(uint32_t seed);

/**
 * @brief Get global random seed (persisted)
 *
 * @return Global seed (defaults to 0xFAB)
 */
uint32_t config_store_get_global_seed(void);

/**
 * @brief Set effective random seed (runtime-only, not persisted)
 *
 * The effective seed is used for actual random operations.
 * Before NTP sync: effective_seed = master_seed XOR true_random
 * After NTP sync: effective_seed = master_seed
 *
 * @param seed Effective seed to use
 */
void config_store_set_effective_seed(uint32_t seed);

/**
 * @brief Get effective random seed (runtime-only)
 *
 * @return Current effective seed (defaults to master seed if not set)
 */
uint32_t config_store_get_effective_seed(void);

// ============================================================================
// Refresh Interval (persisted)
// ============================================================================

/**
 * @brief Set background refresh interval for Makapix channels
 * 
 * @param interval_sec Refresh interval in seconds (default: 3600 = 1 hour)
 * @return ESP_OK on success
 */
esp_err_t config_store_set_refresh_interval_sec(uint32_t interval_sec);

/**
 * @brief Get background refresh interval for Makapix channels
 * 
 * @return Refresh interval in seconds (defaults to 3600 = 1 hour)
 */
uint32_t config_store_get_refresh_interval_sec(void);

// ============================================================================
// Background color (persisted)
// ============================================================================

/**
 * @brief Set background color (persisted in NVS, applies at runtime)
 *
 * Stored in config JSON as:
 *   { "background_color": { "r":0, "g":0, "b":0 } }
 *
 * Defaults to pure black if missing.
 */
esp_err_t config_store_set_background_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Get background color (persisted; cached for runtime use)
 *
 * If not yet loaded, reads from NVS once; defaults to (0,0,0).
 */
void config_store_get_background_color(uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Get a monotonically increasing generation counter for background color
 *
 * Increments whenever background color changes (via setters or config_save()).
 * Decoders can use this to detect runtime changes.
 */
uint32_t config_store_get_background_color_generation(void);

// ============================================================================
// FPS Display (persisted)
// ============================================================================

/**
 * @brief Set whether to show FPS counter on screen
 *
 * @param enable True to show FPS, false to hide
 * @return ESP_OK on success
 */
esp_err_t config_store_set_show_fps(bool enable);

/**
 * @brief Get whether to show FPS counter on screen
 *
 * @return True if FPS display enabled (defaults to true)
 */
bool config_store_get_show_fps(void);

// ============================================================================
// Max Speed Playback (persisted)
// ============================================================================

/**
 * @brief Set max speed playback mode
 *
 * When enabled, frame timing delays are skipped and animations play
 * as fast as the system can decode and render them.
 *
 * @param enable True to enable max speed playback
 * @return ESP_OK on success
 */
esp_err_t config_store_set_max_speed_playback(bool enable);

/**
 * @brief Get max speed playback mode
 *
 * @return True if max speed playback enabled (defaults to false)
 */
bool config_store_get_max_speed_playback(void);

// ============================================================================
// View Acknowledgment (persisted)
// ============================================================================

/**
 * @brief Set view acknowledgment mode
 *
 * When enabled, view events will include "request_ack": true and the player
 * will wait for acknowledgment from the server. Used for debugging.
 *
 * @param enable True to request acknowledgment for view events
 * @return ESP_OK on success
 */
esp_err_t config_store_set_view_ack(bool enable);

/**
 * @brief Get view acknowledgment mode
 *
 * @return True if view acknowledgment enabled (defaults to false)
 */
bool config_store_get_view_ack(void);

// ============================================================================
// SD Card Root Folder (persisted, requires reboot)
// ============================================================================

/**
 * @brief Set SD card root folder path
 *
 * All p3a data directories (animations, vault, channel, etc.) will be
 * created under this root folder. Changes require a reboot to take effect.
 *
 * @param root_path Root folder path - user-friendly format (e.g., "/p3a", "/data")
 * @return ESP_OK on success
 */
esp_err_t config_store_set_sdcard_root(const char *root_path);

/**
 * @brief Get SD card root folder path
 *
 * @param out_path Pointer to receive allocated string (caller must free)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t config_store_get_sdcard_root(char **out_path);

// ============================================================================
// Channel Cache Size (persisted)
// ============================================================================

/**
 * @brief Set channel cache size (max artworks per channel)
 *
 * Controls how many artworks can be cached per channel. Higher values use
 * more memory and disk space. Value is cached in memory for fast access.
 *
 * @param size Cache size (32-4096, default 1024)
 * @return ESP_OK on success
 */
esp_err_t config_store_set_channel_cache_size(uint32_t size);

/**
 * @brief Get channel cache size (max artworks per channel)
 *
 * Returns the configured channel cache size. Uses in-memory caching for
 * performance - NVS is only read once.
 *
 * @return Cache size (defaults to 1024 if not set)
 */
uint32_t config_store_get_channel_cache_size(void);

// ============================================================================
// Processing Notification Settings (persisted)
// ============================================================================

/**
 * @brief Set whether processing notification is enabled
 *
 * When enabled, a checkerboard triangle appears in the bottom-right corner
 * when user initiates an animation swap. Blue during processing, red on failure.
 *
 * @param enable True to show processing notification, false to hide
 * @return ESP_OK on success
 */
esp_err_t config_store_set_proc_notif_enabled(bool enable);

/**
 * @brief Get whether processing notification is enabled
 *
 * @return True if processing notification enabled (defaults to true)
 */
bool config_store_get_proc_notif_enabled(void);

/**
 * @brief Set processing notification size
 *
 * Controls the size of the triangle indicator in pixels.
 * - Size 0: Disables the processing notification indicator
 * - Sizes 1-15: Auto-corrected to 16
 * - Sizes 16-256: Used as-is
 * - Sizes >256: Capped at 256
 *
 * @param size Size in pixels (0=disabled, 16-256, default 64)
 * @return ESP_OK on success
 */
esp_err_t config_store_set_proc_notif_size(uint16_t size);

/**
 * @brief Get processing notification size
 *
 * @return Size in pixels (0=disabled, 16-256, defaults to 64 if not set)
 */
uint16_t config_store_get_proc_notif_size(void);

// ============================================================================
// Shuffle Override (persisted)
// ============================================================================

/**
 * @brief Set shuffle override mode
 *
 * When enabled, forces random pick mode regardless of the playset's pick_mode.
 *
 * @param enable True to enable shuffle override
 * @return ESP_OK on success
 */
esp_err_t config_store_set_shuffle_override(bool enable);

/**
 * @brief Get shuffle override mode
 *
 * @return True if shuffle override enabled (defaults to false)
 */
bool config_store_get_shuffle_override(void);

// ============================================================================
// LTF (Load Tracker File) Enable/Disable
// ============================================================================

/**
 * @brief Set LTF system enabled state
 *
 * When disabled, the LTF system is bypassed entirely - all downloads are
 * allowed and no failures are recorded. Useful for testing.
 *
 * @param enable True to enable LTF, false to disable
 * @return ESP_OK on success
 */
esp_err_t config_store_set_ltf_enabled(bool enable);

/**
 * @brief Get LTF system enabled state
 *
 * Uses in-memory caching for minimal overhead when called frequently.
 *
 * @return True if LTF enabled (defaults to true)
 */
bool config_store_get_ltf_enabled(void);

#ifdef __cplusplus
}
#endif

