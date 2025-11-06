#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *parent;
    lv_obj_t *canvas;  // Canvas widget for animation display
} renderer_config_t;

typedef struct {
    float fps;
    const char *current_animation;
    size_t animation_count;
    size_t current_index;
    bool is_playing;
} renderer_status_t;

/**
 * @brief Initialize the animation renderer
 * 
 * Scans SD card for WebP animation files and sets up the rendering pipeline.
 * 
 * @param config Configuration including parent widget and canvas
 * @return ESP_OK on success
 */
esp_err_t renderer_init(const renderer_config_t *config);

/**
 * @brief Cycle to the next animation
 * 
 * Switches to the next available WebP animation file in the list.
 */
void renderer_cycle_next(void);

/**
 * @brief Check if renderer is ready and has animations loaded
 */
bool renderer_is_ready(void);

/**
 * @brief Get current renderer status
 */
esp_err_t renderer_get_status(renderer_status_t *status);

/**
 * @brief Update function to be called periodically from LVGL tick handler
 * 
 * Decodes and renders the next animation frame.
 */
void renderer_update(void);
