#ifndef PLAYBACK_CONTROLLER_H
#define PLAYBACK_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "animation_metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Active playback source type
 * 
 * Defines what content source is currently active:
 * - NONE: No playback active (UI mode or idle)
 * - PICO8_STREAM: Real-time PICO-8 streaming from WiFi or USB
 * - LOCAL_ANIMATION: Local animation file playback from SD card
 */
typedef enum {
    PLAYBACK_SOURCE_NONE,           // Nothing playing (UI mode or idle)
    PLAYBACK_SOURCE_PICO8_STREAM,   // Live PICO-8 streaming (WiFi/USB)
    PLAYBACK_SOURCE_LOCAL_ANIMATION // Local animation file playback
} playback_source_t;

/**
 * @brief Initialize the playback controller
 * 
 * Must be called after display_renderer_init() and before using other functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t playback_controller_init(void);

/**
 * @brief Deinitialize the playback controller
 */
void playback_controller_deinit(void);

/**
 * @brief Get current playback source
 * 
 * @return Current source type
 */
playback_source_t playback_controller_get_source(void);

/**
 * @brief Switch to PICO-8 streaming mode
 * 
 * Suspends local animation playback and enables PICO-8 rendering.
 * Animation state is preserved for resumption.
 * 
 * @return ESP_OK on success
 */
esp_err_t playback_controller_enter_pico8_mode(void);

/**
 * @brief Exit PICO-8 streaming mode
 * 
 * Resumes local animation playback from where it was suspended.
 */
void playback_controller_exit_pico8_mode(void);

/**
 * @brief Check if system is in PICO-8 streaming mode
 * 
 * @return true if PICO-8 mode is active
 */
bool playback_controller_is_pico8_active(void);

/**
 * @brief Get metadata for currently playing animation
 * 
 * Only valid when playback source is PLAYBACK_SOURCE_LOCAL_ANIMATION.
 * 
 * @param out_meta Pointer to receive metadata pointer (read-only)
 * @return ESP_OK if animation is playing and metadata available
 *         ESP_ERR_INVALID_STATE if not playing local animation
 *         ESP_ERR_INVALID_ARG if out_meta is NULL
 */
esp_err_t playback_controller_get_current_metadata(const animation_metadata_t **out_meta);

/**
 * @brief Update current animation metadata
 * 
 * Called by animation_player when a new animation is loaded.
 * 
 * @param filepath Path to the animation file
 * @param try_load_sidecar If true, attempt to load JSON sidecar
 * @return ESP_OK on success
 */
esp_err_t playback_controller_set_animation_metadata(const char *filepath, bool try_load_sidecar);

/**
 * @brief Clear current animation metadata
 * 
 * Called when animation playback stops or enters non-animation mode.
 */
void playback_controller_clear_metadata(void);

/**
 * @brief Check if current animation has metadata loaded
 * 
 * @return true if has_metadata is true for current animation
 */
bool playback_controller_has_animation_metadata(void);

/**
 * @brief Get metadata field1 (string) for current animation
 * 
 * @return String value or NULL if no metadata or field not set
 */
const char *playback_controller_get_metadata_field1(void);

/**
 * @brief Get metadata field2 (int) for current animation
 * 
 * @return Integer value (0 if no metadata or field not set)
 */
int32_t playback_controller_get_metadata_field2(void);

/**
 * @brief Get metadata field3 (bool) for current animation
 * 
 * @return Boolean value (false if no metadata or field not set)
 */
bool playback_controller_get_metadata_field3(void);

#ifdef __cplusplus
}
#endif

#endif // PLAYBACK_CONTROLLER_H

