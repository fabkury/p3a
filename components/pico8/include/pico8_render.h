#ifndef PICO8_RENDER_H
#define PICO8_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PICO8_FRAME_WIDTH        128
#define PICO8_FRAME_HEIGHT       128
#define PICO8_PALETTE_COLORS     16
#define PICO8_FRAME_BYTES        (PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT / 2)

/**
 * @brief Initialize PICO-8 rendering resources
 * 
 * Allocates frame buffers and lookup tables.
 * Called automatically on first frame submission.
 * 
 * @return ESP_OK on success
 */
esp_err_t pico8_render_init(void);

/**
 * @brief Release PICO-8 rendering resources
 */
void pico8_render_deinit(void);

/**
 * @brief Check if PICO-8 resources are initialized
 * 
 * @return true if initialized
 */
bool pico8_render_is_initialized(void);

/**
 * @brief Submit a PICO-8 frame for rendering
 * 
 * Decodes packed 4bpp pixel data using the provided or default palette,
 * and stores the result for display by the render loop.
 * 
 * @param palette_rgb RGB palette data (48 bytes for 16 colors), or NULL to use current palette
 * @param palette_len Length of palette data (should be 48 if provided)
 * @param pixel_data Packed 4bpp pixel data (8192 bytes)
 * @param pixel_len Length of pixel data
 * @return ESP_OK on success
 */
esp_err_t pico8_render_submit_frame(const uint8_t *palette_rgb, size_t palette_len,
                                    const uint8_t *pixel_data, size_t pixel_len);

/**
 * @brief Render a PICO-8 frame to the display buffer
 * 
 * Called by the render loop when PICO-8 mode is active.
 * 
 * @param dest_buffer Destination display buffer
 * @param row_stride Row stride in bytes
 * @return Frame delay in milliseconds (typically 16ms for 60fps)
 */
int pico8_render_frame(uint8_t *dest_buffer, size_t row_stride);

/**
 * @brief Check if a PICO-8 frame is ready for rendering
 * 
 * @return true if a frame has been submitted and is ready
 */
bool pico8_render_frame_ready(void);

/**
 * @brief Mark frame as consumed (called after rendering)
 */
void pico8_render_mark_consumed(void);

/**
 * @brief Render the PICO-8 logo (shown when waiting for stream)
 * 
 * @param dest_buffer Destination display buffer
 * @param row_stride Row stride in bytes
 * @return Frame delay in milliseconds
 */
int pico8_render_logo(uint8_t *dest_buffer, size_t row_stride);

/**
 * @brief Get timestamp of last frame
 * 
 * @return Timestamp in microseconds, or 0 if no frame received
 */
int64_t pico8_render_get_last_frame_time(void);

#ifdef __cplusplus
}
#endif

#endif // PICO8_RENDER_H

