// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "render_engine.h"
#include "config_store.h"

esp_err_t render_engine_set_rotation(display_rotation_t rotation)
{
    return display_renderer_set_rotation(rotation);
}

esp_err_t render_engine_set_background(uint8_t r, uint8_t g, uint8_t b)
{
    return config_store_set_background_color(r, g, b);
}
