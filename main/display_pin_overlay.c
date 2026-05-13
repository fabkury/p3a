// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file display_pin_overlay.c
 * @brief Pin overlay: shows pin/error icon in the user's TOP-LEFT corner
 *
 * Mirror of display_reaction_overlay.c but positioned on the opposite corner
 * and driven by its own independent state machine. The two overlays can show
 * simultaneously — a swipe-up on a Makapix artwork triggers both the
 * reaction overlay (top-right) and the pin overlay (top-left).
 *
 * Triggered by the pin dispatcher (p3a_pin_dispatcher.c). On success the
 * pin icon shows for 4 s; on failure the icon is replaced (with a fresh
 * timer) by the error icon. The error variant reuses reaction_error_img.
 */

#include "display_renderer_priv.h"
#include "pin_icon_img.h"
#include "reaction_error_img.h"

#define PIN_OVERLAY_DURATION_MS    4000
#define PIN_OVERLAY_CENTER_INSET   32   // Distance from user-visual edges to image center

typedef void (*pin_blit_fn_t)(
    uint8_t *dst, int dst_w, int dst_h, int dst_stride_bytes,
    int x, int y,
    uint8_t alpha, uint8_t bg_b, uint8_t bg_g, uint8_t bg_r,
    int scale, int rotation);

void pin_overlay_show_submit(void)
{
    if (g_pin_overlay_state == PIN_OVERLAY_SUBMIT) {
        return;  // Dedup: already showing pin success
    }
    g_pin_overlay_state = PIN_OVERLAY_SUBMIT;
    g_pin_overlay_start_us = esp_timer_get_time();
}

void pin_overlay_show_error(void)
{
    // No dedup: error always restarts the 4 s timer so it gets its own window.
    g_pin_overlay_state = PIN_OVERLAY_ERROR;
    g_pin_overlay_start_us = esp_timer_get_time();
}

void pin_overlay_update_and_draw(uint8_t *buffer)
{
    if (!buffer) return;
    if (g_pin_overlay_state == PIN_OVERLAY_IDLE) return;

    int64_t elapsed_us = esp_timer_get_time() - g_pin_overlay_start_us;
    if (elapsed_us > (int64_t)PIN_OVERLAY_DURATION_MS * 1000LL) {
        g_pin_overlay_state = PIN_OVERLAY_IDLE;
        g_pin_overlay_start_us = 0;
        return;
    }

    pin_blit_fn_t blit_fn;
    int img_w, img_h;
    switch (g_pin_overlay_state) {
        case PIN_OVERLAY_SUBMIT:
            blit_fn = pin_icon_img_blit_pixelwise_bgr888;
            img_w = pin_icon_img_w;
            img_h = pin_icon_img_h;
            break;
        case PIN_OVERLAY_ERROR:
            blit_fn = reaction_error_img_blit_pixelwise_bgr888;
            img_w = reaction_error_img_w;
            img_h = reaction_error_img_h;
            break;
        default:
            return;
    }

    const int C = PIN_OVERLAY_CENTER_INSET;
    const int sw = EXAMPLE_LCD_H_RES;
    const int sh = EXAMPLE_LCD_V_RES;
    int blit_x, blit_y, rotation_deg;

    // Mirror of display_reaction_overlay.c, but the target is the user's
    // TOP-LEFT corner instead of TOP-RIGHT. The user's top-left maps to phys
    // (0,0) at ROT_0, (W-1,0) at ROT_90, (W-1,H-1) at ROT_180, (0,H-1) at
    // ROT_270. The blit functions swap (img_w,img_h) under rotation 90/270.
    switch (g_screen_rotation) {
        default:
        case DISPLAY_ROTATION_0:
            // User top-left = phys top-left. Box is (img_w x img_h).
            blit_x = C - img_w / 2;
            blit_y = C - img_h / 2;
            rotation_deg = 0;
            break;
        case DISPLAY_ROTATION_90:
            // User top-left = phys top-right. Rotated box is (img_h x img_w).
            blit_x = sw - C - img_h / 2;
            blit_y = C - img_w / 2;
            rotation_deg = 90;
            break;
        case DISPLAY_ROTATION_180:
            // User top-left = phys bottom-right. Box is (img_w x img_h).
            blit_x = sw - C - img_w / 2;
            blit_y = sh - C - img_h / 2;
            rotation_deg = 180;
            break;
        case DISPLAY_ROTATION_270:
            // User top-left = phys bottom-left. Rotated box is (img_h x img_w).
            blit_x = C - img_h / 2;
            blit_y = sh - C - img_w / 2;
            rotation_deg = 270;
            break;
    }

    blit_fn(buffer, sw, sh, (int)g_display_row_stride,
            blit_x, blit_y,
            255, 0, 0, 0,
            1, rotation_deg);
}
