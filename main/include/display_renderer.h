#ifndef DISPLAY_RENDERER_H
#define DISPLAY_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Screen rotation angles
 */
typedef enum {
    DISPLAY_ROTATION_0   = 0,
    DISPLAY_ROTATION_90  = 90,
    DISPLAY_ROTATION_180 = 180,
    DISPLAY_ROTATION_270 = 270
} display_rotation_t;

/**
 * @brief Render mode for the display
 */
typedef enum {
    DISPLAY_RENDER_MODE_ANIMATION,  // Animation/streaming pipeline owns buffers
    DISPLAY_RENDER_MODE_UI          // UI pipeline owns buffers
} display_render_mode_t;

/**
 * @brief Frame source callback type
 * 
 * The render loop calls this to get the next frame to display.
 * Return value is the frame delay in milliseconds (or negative on error).
 */
typedef int (*display_frame_callback_t)(uint8_t *dest_buffer, void *user_ctx);

/**
 * @brief Initialize display renderer with LCD panel
 * 
 * @param panel LCD panel handle
 * @param buffers Array of frame buffer pointers
 * @param buffer_count Number of buffers (typically 2 for double buffering)
 * @param buffer_bytes Size of each buffer in bytes
 * @param row_stride Row stride in bytes
 * @return ESP_OK on success
 */
esp_err_t display_renderer_init(esp_lcd_panel_handle_t panel,
                                uint8_t **buffers,
                                uint8_t buffer_count,
                                size_t buffer_bytes,
                                size_t row_stride);

/**
 * @brief Deinitialize display renderer
 */
void display_renderer_deinit(void);

/**
 * @brief Start the render loop task
 * 
 * @return ESP_OK on success
 */
esp_err_t display_renderer_start(void);

/**
 * @brief Set the frame source callback for animation/streaming mode
 * 
 * @param callback Function to call each frame
 * @param user_ctx User context passed to callback
 */
void display_renderer_set_frame_callback(display_frame_callback_t callback, void *user_ctx);

/**
 * @brief Enter UI rendering mode
 * 
 * Blocks until render loop acknowledges mode switch.
 * 
 * @return ESP_OK on success
 */
esp_err_t display_renderer_enter_ui_mode(void);

/**
 * @brief Exit UI rendering mode
 * 
 * Blocks until render loop acknowledges mode switch.
 */
void display_renderer_exit_ui_mode(void);

/**
 * @brief Check if currently in UI mode
 * 
 * @return true if UI mode active
 */
bool display_renderer_is_ui_mode(void);

/**
 * @brief Set screen rotation
 * 
 * @param rotation Rotation angle
 * @return ESP_OK on success
 */
esp_err_t display_renderer_set_rotation(display_rotation_t rotation);

/**
 * @brief Get current screen rotation
 * 
 * @return Current rotation angle
 */
display_rotation_t display_renderer_get_rotation(void);

/**
 * @brief Get frame buffer dimensions and stride
 * 
 * @param width Output: horizontal resolution
 * @param height Output: vertical resolution
 * @param stride Output: row stride in bytes
 */
void display_renderer_get_dimensions(int *width, int *height, size_t *stride);

/**
 * @brief Get frame buffer byte size
 * 
 * @return Size of one frame buffer in bytes
 */
size_t display_renderer_get_buffer_bytes(void);

/**
 * @brief Trigger parallel upscale operation using worker tasks
 * 
 * Sets up shared state and notifies upscale workers to process rows.
 * Blocks until both workers complete.
 * 
 * @param src_rgba Source RGBA buffer
 * @param src_w Source width
 * @param src_h Source height
 * @param dst_buffer Destination buffer
 * @param lookup_x X lookup table
 * @param lookup_y Y lookup table
 * @param rotation Rotation to apply
 */
void display_renderer_parallel_upscale(const uint8_t *src_rgba, int src_w, int src_h,
                                       uint8_t *dst_buffer,
                                       const uint16_t *lookup_x, const uint16_t *lookup_y,
                                       display_rotation_t rotation);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_RENDERER_H

