// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* typewriter: source-columns of the rotated logo are revealed left-to-right
 * with a contrast-color cursor block at the head. The cursor blinks at a
 * fixed cadence and trails off as the head reaches the right edge.
 *
 * Reveal pacing is linear so the cursor moves at constant speed (more
 * "typewriter" than smoothstep-paced); blink is a square wave at 6 Hz of
 * normalized time. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_TW_BLINK_CYCLES  6.0f   /* full on/off cycles across t=[0,1] */

void ia_typewriter_render(uint8_t *buffer,
                          const intro_anim_ctx_t *ctx,
                          float t)
{
    if (t <= 0.0f) {
        intro_anim_fill_bg(buffer, ctx);
        return;
    }
    if (t >= 1.0f) {
        intro_anim_fill_bg(buffer, ctx);
        p3a_logo_blit_pixelwise_bgr888(
            buffer, ctx->width, ctx->height, (int)ctx->stride,
            ctx->logo_x, ctx->logo_y, 255,
            ctx->bg_b, ctx->bg_g, ctx->bg_r,
            ctx->logo_scale, ctx->rotation);
        return;
    }

    intro_anim_fill_bg(buffer, ctx);

    intro_anim_rot_t r;
    intro_anim_rot_init(&r, ctx->rotation);
    const int scale = ctx->logo_scale;

    /* Cursor head in source-column units. Linear pacing. */
    int head_src = (int)((float)r.rotated_w * t);
    if (head_src < 0) head_src = 0;
    if (head_src >= r.rotated_w) head_src = r.rotated_w - 1;

    /* Contrast color picked against bg luminance. */
    int lum = (int)ctx->bg_r * 299 + (int)ctx->bg_g * 587 + (int)ctx->bg_b * 114;
    uint8_t cur_v = (lum < 127500) ? 240 : 24;

    /* Blink: phase in [0, IA_TW_BLINK_CYCLES); on for first half of each. */
    float phase = t * IA_TW_BLINK_CYCLES;
    int phase_i = (int)phase;
    int blink_on = ((phase - (float)phase_i) < 0.5f);

    /* Draw revealed columns 0..head_src-1 fully. */
    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < head_src; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            int dx0 = ctx->logo_x + ox * scale;
            int dy0 = ctx->logo_y + oy * scale;
            for (int by = 0; by < scale; by++) {
                int dy = dy0 + by;
                if (dy < 0 || dy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)dy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int dx = dx0 + bx;
                    if (dx < 0 || dx >= ctx->width) continue;
                    uint8_t *dst = row + dx * 3;
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                }
            }
        }
    }

    /* Draw cursor column at head_src (full bbox height in dest), blinking. */
    if (blink_on) {
        int dx0 = ctx->logo_x + head_src * scale;
        int dy0 = ctx->logo_y;
        int rh = r.rotated_h * scale;
        for (int by = 0; by < rh; by++) {
            int dy = dy0 + by;
            if (dy < 0 || dy >= ctx->height) continue;
            uint8_t *row = buffer + (size_t)dy * ctx->stride;
            for (int bx = 0; bx < scale; bx++) {
                int dx = dx0 + bx;
                if (dx < 0 || dx >= ctx->width) continue;
                uint8_t *dst = row + dx * 3;
                dst[0] = cur_v; dst[1] = cur_v; dst[2] = cur_v;
            }
        }
    }
}
