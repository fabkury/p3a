// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file render_engine.h
 * @brief Render engine interface for rotation and background control
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "display_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    int delay_ms;
} raw_frame_t;

typedef struct {
    uint8_t *buffer;
    size_t stride;
} output_frame_t;

esp_err_t render_engine_set_rotation(display_rotation_t rotation);
esp_err_t render_engine_set_background(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
