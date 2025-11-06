#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FILE_GIF = 0,
    FILE_WEBP = 1,
} anim_type_t;

typedef struct {
    anim_type_t type;
    const char* path;        // Absolute path on SD card
    int native_size_px;      // 16, 32, 64, or 128 (must be validated)
} anim_desc_t;

/**
 * @brief Initialize player system
 * 
 * Allocates buffers, creates tasks, initializes decoders.
 * Must be called before using player functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t player_init(void);

/**
 * @brief Start playing an animation
 * 
 * Opens the file, starts decoder and renderer tasks.
 * 
 * @param desc Animation descriptor (type, path, native_size_px)
 * @return true on success, false on error
 */
bool player_start(const anim_desc_t* desc);

/**
 * @brief Stop playing current animation
 * 
 * Stops decoder and renderer tasks, closes file.
 */
void player_stop(void);

/**
 * @brief Check if player is currently running
 * 
 * @return true if playing, false otherwise
 */
bool player_is_running(void);

#ifdef __cplusplus
}
#endif

