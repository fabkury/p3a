// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file display_reaction_overlay.c
 * @brief Reaction overlay: shows thumbs-up/down/error icon in top-right corner
 *
 * Triggered by swipe-up (submit) or swipe-down (revoke) gestures when viewing
 * Makapix Club artwork. The submit/revoke icons appear optimistically the
 * moment the gesture is recognized; if the MQTT publish subsequently fails,
 * the overlay is overridden by an error icon (with a fresh 4 s timer).
 *
 * The overlay follows screen rotation; its center pixel sits
 * REACTION_OVERLAY_CENTER_INSET pixels from each user-visual edge.
 */

#include "display_renderer_priv.h"
#include "reaction_submit_img.h"
#include "reaction_revoke_img.h"
#include "reaction_error_img.h"

#define REACTION_OVERLAY_DURATION_MS    4000
#define REACTION_OVERLAY_CENTER_INSET   32   // Distance from user-visual edges to image center

typedef void (*reaction_blit_fn_t)(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes,
    int x, int y,
    uint8_t alpha, uint8_t bg_b, uint8_t bg_g, uint8_t bg_r,
    int scale, int rotation);

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

void reaction_overlay_show_error(void)
{
    // No dedup: the error always (re)starts the 4 s timer so it gets its own
    // full display window regardless of what was showing before.
    g_reaction_overlay_state = REACTION_OVERLAY_ERROR;
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

    // Select the active image's blit function and native dimensions. The
    // pixelwise blit swaps its bounding box when rotation is 90/270, so the
    // on-buffer footprint is (img_w x img_h) for 0/180 and (img_h x img_w)
    // for 90/270.
    reaction_blit_fn_t blit_fn;
    int img_w, img_h;
    switch (g_reaction_overlay_state) {
        case REACTION_OVERLAY_SUBMIT:
            blit_fn = reaction_submit_img_blit_pixelwise_bgr888;
            img_w = reaction_submit_img_w;
            img_h = reaction_submit_img_h;
            break;
        case REACTION_OVERLAY_REVOKE:
            blit_fn = reaction_revoke_img_blit_pixelwise_bgr888;
            img_w = reaction_revoke_img_w;
            img_h = reaction_revoke_img_h;
            break;
        case REACTION_OVERLAY_ERROR:
            blit_fn = reaction_error_img_blit_pixelwise_bgr888;
            img_w = reaction_error_img_w;
            img_h = reaction_error_img_h;
            break;
        default:
            return;
    }

    const int C = REACTION_OVERLAY_CENTER_INSET;
    const int sw = EXAMPLE_LCD_H_RES;
    const int sh = EXAMPLE_LCD_V_RES;
    int blit_x, blit_y, rotation_deg;

    // Convention (matches display_upscaler.c / display_processing_notification.c):
    //   ROT_0:   user-up = phys-up,    user-right = phys-right
    //   ROT_90:  user-up = phys-right, user-right = phys-down
    //   ROT_180: user-up = phys-down,  user-right = phys-left
    //   ROT_270: user-up = phys-left,  user-right = phys-up
    // So user's top-right corner maps to phys (W-1,0), (W-1,H-1), (0,H-1), (0,0).
    switch (g_screen_rotation) {
        default:
        case DISPLAY_ROTATION_0:
            // User top-right = phys top-right. Box is (img_w x img_h).
            blit_x = sw - C - img_w / 2;
            blit_y = C - img_h / 2;
            rotation_deg = 0;
            break;
        case DISPLAY_ROTATION_90:
            // User top-right = phys bottom-right. Rotated box is (img_h x img_w).
            blit_x = sw - C - img_h / 2;
            blit_y = sh - C - img_w / 2;
            rotation_deg = 90;
            break;
        case DISPLAY_ROTATION_180:
            // User top-right = phys bottom-left. Box is (img_w x img_h).
            blit_x = C - img_w / 2;
            blit_y = sh - C - img_h / 2;
            rotation_deg = 180;
            break;
        case DISPLAY_ROTATION_270:
            // User top-right = phys top-left. Rotated box is (img_h x img_w).
            blit_x = C - img_h / 2;
            blit_y = C - img_w / 2;
            rotation_deg = 270;
            break;
    }

    // alpha=255 takes the opaque code path; the chroma-key check in the
    // generated blit skips magenta pixels so the underlying frame shows
    // through (bg args are therefore unused).
    blit_fn(buffer, sw, sh, (int)g_display_row_stride,
            blit_x, blit_y,
            255, 0, 0, 0,
            1, rotation_deg);
}
