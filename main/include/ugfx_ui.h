#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "gfx.h"  // For gBool type
#include "animation_player.h"  // For screen_rotation_t

/**
 * @brief Initialize µGFX UI system
 * 
 * @return ESP_OK on success
 */
esp_err_t ugfx_ui_init(void);

/**
 * @brief Deinitialize µGFX UI system
 * 
 * Cleans up µGFX resources.
 */
void ugfx_ui_deinit(void);

/**
 * @brief Show provisioning status with a message
 * 
 * Displays a loading screen with a status message during provisioning.
 * 
 * @param status_message Status message to display (e.g., "Querying endpoint")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ugfx_ui_show_provisioning_status(const char *status_message);

/**
 * @brief Show captive portal AP information screen
 * 
 * Displays instructions on how to connect to the captive portal and
 * configure WiFi settings.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ugfx_ui_show_captive_ap_info(void);

/**
 * @brief Activate registration code display
 * 
 * Sets up the UI state for displaying a registration code.
 * The actual rendering happens when ugfx_ui_render_to_buffer() is called.
 * 
 * @param code 6-character registration code
 * @param expires_at ISO 8601 timestamp string for expiration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ugfx_ui_show_registration(const char *code, const char *expires_at);

/**
 * @brief Deactivate registration code display
 * 
 * Clears UI state. The screen will show animation on next frame.
 */
void ugfx_ui_hide_registration(void);

/**
 * @brief Show OTA update progress screen
 * 
 * Displays firmware update progress with a progress bar.
 * Call ugfx_ui_update_ota_progress() to update the displayed progress.
 * 
 * @param version_from Current firmware version (can be NULL)
 * @param version_to Target firmware version (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t ugfx_ui_show_ota_progress(const char *version_from, const char *version_to);

/**
 * @brief Update OTA progress display
 * 
 * Updates the progress percentage and status text shown on screen.
 * 
 * @param percent Progress percentage (0-100)
 * @param status_text Status text to display (e.g., "Downloading...", "Verifying...")
 */
void ugfx_ui_update_ota_progress(int percent, const char *status_text);

/**
 * @brief Hide OTA progress screen
 * 
 * Deactivates the OTA progress UI and returns to normal animation mode.
 */
void ugfx_ui_hide_ota_progress(void);

/**
 * @brief Show channel loading message
 * 
 * Displays a status message about channel loading/downloading progress.
 * 
 * @param channel_name Name of the channel being loaded
 * @param message Status message (e.g., "Loading...", "Downloading artwork...")
 * @param progress_percent Progress percentage (0-100), or -1 if indeterminate
 */
esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent);

/**
 * @brief Hide channel message
 * 
 * Deactivates the channel message UI and returns to normal animation mode.
 */
void ugfx_ui_hide_channel_message(void);

/**
 * @brief Check if UI is currently active
 * 
 * @return gTrue if UI is active, gFalse otherwise
 */
gBool ugfx_ui_is_active(void);

/**
 * @brief Render UI frame to the provided buffer
 * 
 * Called by the animation task when in UI mode. Draws the entire UI
 * (including current countdown timer) to the provided buffer.
 * 
 * @param buffer Pointer to the back buffer to draw into
 * @param stride Row stride in bytes
 * @return Frame delay in ms (100 for UI), or -1 on error
 */
int ugfx_ui_render_to_buffer(uint8_t *buffer, size_t stride);

/**
 * @brief Set µGFX UI rotation
 * 
 * Sets the display orientation for µGFX rendering. Must be called after
 * µGFX is initialized. Takes effect immediately.
 * 
 * @param rotation Screen rotation angle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ugfx_ui_set_rotation(screen_rotation_t rotation);


