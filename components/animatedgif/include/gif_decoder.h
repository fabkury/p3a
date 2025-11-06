#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"
// Note: AnimatedGIF.h not included here as it's C++ only
// The gif pointer is opaque (void*) for C compatibility

#ifdef __cplusplus
extern "C" {
#endif

// GIF decoder state
typedef struct {
    void *gif;  // Opaque pointer to AnimatedGIF (C++ class)
    FILE *file_handle;
    char *file_path;
    uint8_t *memory_data;
    size_t memory_size;
    bool is_playing;
    bool should_loop;
    bool is_paused;
    int loop_count;
    int current_frame_delay_ms;
    int canvas_width;
    int canvas_height;
    void *user_data;
    
    // Callbacks
    void (*on_frame_decoded)(void *user_data, uint8_t *pixels, int width, int height, int delay_ms);
} gif_decoder_state_t;

typedef struct {
    gif_decoder_state_t *decoder_state;
    uint8_t *stripe_buffer;
    int stripe_y;
    int stripe_height;
    int display_width;
    int display_height;
    uint8_t *frame_buffer;
    int frame_width;
    int frame_height;
} gif_draw_context_t;

/**
 * @brief Initialize GIF decoder
 * 
 * @param state Decoder state structure
 * @return ESP_OK on success
 */
esp_err_t gif_decoder_init(gif_decoder_state_t *state);

/**
 * @brief Open a GIF file from path
 * 
 * @param state Decoder state
 * @param file_path Path to GIF file
 * @return ESP_OK on success
 */
esp_err_t gif_decoder_open_file(gif_decoder_state_t *state, const char *file_path);

/**
 * @brief Open a GIF from memory buffer
 * 
 * @param state Decoder state
 * @param data Buffer containing GIF data
 * @param size Size of buffer
 * @return ESP_OK on success
 */
esp_err_t gif_decoder_open_memory(gif_decoder_state_t *state, const uint8_t *data, size_t size);

/**
 * @brief Play the next frame
 * 
 * @param state Decoder state
 * @param delay_ms_out Output parameter for frame delay in milliseconds
 * @return true if frame decoded successfully, false if end of animation
 */
bool gif_decoder_play_frame(gif_decoder_state_t *state, int *delay_ms_out);

/**
 * @brief Reset decoder to beginning
 * 
 * @param state Decoder state
 */
void gif_decoder_reset(gif_decoder_state_t *state);

/**
 * @brief Close decoder and free resources
 * 
 * @param state Decoder state
 */
void gif_decoder_close(gif_decoder_state_t *state);

/**
 * @brief Get canvas dimensions
 * 
 * @param state Decoder state
 * @param width_out Output width
 * @param height_out Output height
 */
void gif_decoder_get_canvas_size(gif_decoder_state_t *state, int *width_out, int *height_out);

/**
 * @brief Get loop count (0 = infinite)
 * 
 * @param state Decoder state
 * @return Loop count
 */
int gif_decoder_get_loop_count(gif_decoder_state_t *state);

/**
 * @brief Set loop mode
 * 
 * @param state Decoder state
 * @param loop true to loop, false to play once
 */
void gif_decoder_set_loop(gif_decoder_state_t *state, bool loop);

/**
 * @brief Set draw context for stripe-based rendering
 * 
 * @param state Decoder state
 * @param draw_context Context containing stripe buffer info
 */
void gif_decoder_set_draw_context(gif_decoder_state_t *state, gif_draw_context_t *draw_context);

#ifdef __cplusplus
}
#endif

