#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Animation format types
typedef enum {
    ANIM_FORMAT_WEBP = 0,
    ANIM_FORMAT_GIF,
    ANIM_FORMAT_UNKNOWN
} anim_format_t;

/**
 * @brief Initialize video player (allocates stripe buffers early)
 * 
 * This should be called very early at boot, before Wi-Fi/BLE/loggers
 * to avoid memory fragmentation.
 * 
 * @return ESP_OK on success
 */
esp_err_t video_player_init(void);

/**
 * @brief Play WebP animation file, bypassing LVGL
 * 
 * When playing, LVGL is suspended and the panel is driven directly
 * using DMA-fed MIPI-DSI with internal SRAM stripes (no tearing).
 * 
 * @param file_data WebP animation file data
 * @param file_size Size of file_data in bytes
 * @param loop If true, loop the animation
 * @return ESP_OK on success
 */
esp_err_t video_player_play_webp(const uint8_t* file_data, size_t file_size, bool loop);

/**
 * @brief Play animation file from path (auto-detects format)
 * 
 * Supports both WebP and GIF formats. The format is auto-detected
 * based on file extension or header.
 * 
 * @param file_path Path to animation file
 * @param loop If true, loop the animation
 * @return ESP_OK on success
 */
esp_err_t video_player_play_file(const char *file_path, bool loop);

/**
 * @brief Play GIF animation file
 * 
 * @param file_path Path to GIF file
 * @param loop If true, loop the animation
 * @return ESP_OK on success
 */
esp_err_t video_player_play_gif(const char *file_path, bool loop);

/**
 * @brief Stop video playback and resume LVGL
 * 
 * @param keep_bypass If true, keep LVGL bypass mode active (for seamless switching)
 * @return ESP_OK on success
 */
esp_err_t video_player_stop(bool keep_bypass);

/**
 * @brief Pause video playback
 * 
 * @return ESP_OK on success
 */
esp_err_t video_player_pause(void);

/**
 * @brief Resume paused video playback
 * 
 * @return ESP_OK on success
 */
esp_err_t video_player_resume(void);

/**
 * @brief Check if video is currently playing
 * 
 * @return true if playing, false otherwise
 */
bool video_player_is_playing(void);

/**
 * @brief Get current playback statistics
 * 
 * @param fps_out Output FPS (can be NULL)
 * @param decode_ms_out Decode time per stripe in ms (can be NULL)
 * @param dma_ms_out DMA time per stripe in ms (can be NULL)
 * @param frame_ms_out Total frame time in ms (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t video_player_get_stats(float *fps_out, float *decode_ms_out, 
                                 float *dma_ms_out, float *frame_ms_out);

/**
 * @brief Detect animation format from file extension
 * 
 * @param file_path File path
 * @return Animation format or ANIM_FORMAT_UNKNOWN
 */
anim_format_t video_player_detect_format(const char *file_path);

#ifdef __cplusplus
}
#endif
