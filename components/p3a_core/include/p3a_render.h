/**
 * @file p3a_render.h
 * @brief State-aware rendering dispatch
 * 
 * Each p3a state has its own render function that produces frames:
 * 
 * ANIMATION_PLAYBACK:
 * - Sub-state PLAYING: delegates to animation decoder
 * - Sub-state CHANNEL_MESSAGE: renders status text (fetching, downloading, etc.)
 * 
 * PROVISIONING:
 * - Sub-state STATUS: renders "PROVISIONING" with status message
 * - Sub-state SHOW_CODE: renders registration code with countdown
 * - Sub-state CAPTIVE_AP_INFO: renders WiFi setup instructions
 * 
 * OTA:
 * - Renders progress bar with version info
 * 
 * PICO8_STREAMING:
 * - No rendering (PICO-8 frames come from USB/WiFi)
 */

#ifndef P3A_RENDER_H
#define P3A_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render result structure
 */
typedef struct {
    int frame_delay_ms;             ///< Suggested delay before next frame (-1 = no delay/immediate)
    bool buffer_modified;           ///< Whether the buffer was modified
} p3a_render_result_t;

/**
 * @brief Initialize render system
 * 
 * Must be called after p3a_state_init() and after display is initialized.
 * 
 * @return ESP_OK on success
 */
esp_err_t p3a_render_init(void);

/**
 * @brief Render current state to buffer
 * 
 * Called by the animation/render task. Dispatches to appropriate
 * state-specific render function.
 * 
 * @param buffer Framebuffer to render into
 * @param stride Row stride in bytes
 * @param result Output render result
 * @return ESP_OK on success
 */
esp_err_t p3a_render_frame(uint8_t *buffer, size_t stride, p3a_render_result_t *result);

/**
 * @brief Check if current state needs rendering
 * 
 * PICO8_STREAMING returns false (external frames).
 * All other states return true.
 * 
 * @return true if p3a_render_frame() should be called
 */
bool p3a_render_needs_frame(void);

/**
 * @brief Set channel message for display
 * 
 * Used by channel loading logic to update the message shown during
 * CHANNEL_MESSAGE sub-state.
 * 
 * @param channel_name Name of channel being loaded
 * @param msg_type Type of message
 * @param progress_percent Download progress (-1 if unknown)
 * @param detail Additional detail text (can be NULL)
 */
void p3a_render_set_channel_message(const char *channel_name,
                                     int msg_type,
                                     int progress_percent,
                                     const char *detail);

/**
 * @brief Set provisioning status message
 * 
 * @param status Status text to display
 */
void p3a_render_set_provisioning_status(const char *status);

/**
 * @brief Set provisioning code for display
 * 
 * @param code 6-character registration code
 * @param expires_at ISO 8601 expiration timestamp
 */
void p3a_render_set_provisioning_code(const char *code, const char *expires_at);

/**
 * @brief Set OTA progress for display
 * 
 * @param percent Progress (0-100)
 * @param status Status text
 * @param version_from Current version
 * @param version_to Target version
 */
void p3a_render_set_ota_progress(int percent, const char *status,
                                  const char *version_from, const char *version_to);

#ifdef __cplusplus
}
#endif

#endif // P3A_RENDER_H

