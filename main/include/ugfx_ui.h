#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "gfx.h"  // For gBool type

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

// Forward declaration for screen_rotation_t
typedef enum {
    ROTATION_0   = 0,
    ROTATION_90  = 90,
    ROTATION_180 = 180,
    ROTATION_270 = 270
} screen_rotation_t;

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


