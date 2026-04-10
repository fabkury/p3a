// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file display_reaction_overlay.c
 * @brief Reaction overlay: shows thumbs-up/down icon in top-right corner for 3 seconds
 *
 * Triggered by swipe-up (submit) or swipe-down (revoke) gestures when viewing
 * Makapix Club artwork. The overlay follows screen rotation.
 */

#include "display_renderer_priv.h"
#include "reaction_submit_img.h"
#include "reaction_revoke_img.h"

#define REACTION_OVERLAY_DURATION_MS  3000
#define REACTION_OVERLAY_MARGIN       4
#define REACTION_OVERLAY_SIZE         64  // Both images are 64x64

void reaction_overlay_show_submit(void)
{
    if (g_reaction_overlay_state == REACTION_OVERLAY_SUBMIT) {
        return;  // Dedup: already showing submit
    }
    g_reaction_overlay_state = REACTION_OVERLAY_SUBMIT;
    g_reaction_overlay_start_us = esp_timer_get_time();
}

void reaction_overlay_show_revoke(void)
{
    if (g_reaction_overlay_state == REACTION_OVERLAY_REVOKE) {
        return;  // Dedup: already showing revoke
    }
    g_reaction_overlay_state = REACTION_OVERLAY_REVOKE;
    g_reaction_overlay_start_us = esp_timer_get_time();
}

void reaction_overlay_update_and_draw(uint8_t *buffer)
{
    if (!buffer) return;
    if (g_reaction_overlay_state == REACTION_OVERLAY_IDLE) return;

    // Check if duration has elapsed
    int64_t elapsed_us = esp_timer_get_time() - g_reaction_overlay_start_us;
    if (elapsed_us > (int64_t)REACTION_OVERLAY_DURATION_MS * 1000LL) {
        g_reaction_overlay_state = REACTION_OVERLAY_IDLE;
        g_reaction_overlay_start_us = 0;
        return;
    }

    // Compute top-right position in user's visual space, mapped to physical coords.
    // For a square image (64x64), rotation doesn't change output dimensions.
    const int img_size = REACTION_OVERLAY_SIZE;
    const int margin = REACTION_OVERLAY_MARGIN;
    const int screen_w = EXAMPLE_LCD_H_RES;
    const int screen_h = EXAMPLE_LCD_V_RES;
    int blit_x, blit_y, rotation_deg;

    switch (g_screen_rotation) {
        default:
        case DISPLAY_ROTATION_0:
            // Top-right: image at far right, near top
            blit_x = screen_w - img_size - margin;
            blit_y = margin;
            rotation_deg = 0;
            break;
        case DISPLAY_ROTATION_90:
            // User's top-right maps to physical top-left
            blit_x = margin;
            blit_y = margin;
            rotation_deg = 90;
            break;
        case DISPLAY_ROTATION_180:
            // User's top-right maps to physical bottom-left
            blit_x = margin;
            blit_y = screen_h - img_size - margin;
            rotation_deg = 180;
            break;
        case DISPLAY_ROTATION_270:
            // User's top-right maps to physical bottom-right
            blit_x = screen_w - img_size - margin;
            blit_y = screen_h - img_size - margin;
            rotation_deg = 270;
            break;
    }

    // Blit the appropriate image with chroma-key transparency (alpha=255, scale=1)
    if (g_reaction_overlay_state == REACTION_OVERLAY_SUBMIT) {
        reaction_submit_img_blit_pixelwise_bgr888(
            buffer, screen_w, screen_h, (int)g_display_row_stride,
            blit_x, blit_y,
            255, 0, 0, 0,  // alpha=255 (opaque), bg color unused with chroma key
            1, rotation_deg
        );
    } else {
        reaction_revoke_img_blit_pixelwise_bgr888(
            buffer, screen_w, screen_h, (int)g_display_row_stride,
            blit_x, blit_y,
            255, 0, 0, 0,
            1, rotation_deg
        );
    }
}
