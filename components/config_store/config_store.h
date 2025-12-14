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
 * @brief Set playlist expansion (PE)
 * 
 * @param pe Playlist expansion (0-1023, 0 = infinite)
 * @return ESP_OK on success
 */
esp_err_t config_store_set_pe(uint32_t pe);

/**
 * @brief Get playlist expansion (PE)
 * 
 * @return PE value (defaults to 8 if not set)
 */
uint32_t config_store_get_pe(void);

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

#ifdef __cplusplus
}
#endif

