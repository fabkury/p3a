// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* bayer-reveal: ordered-dither threshold reveal. An 8x8 Bayer matrix
 * gives every source pixel a deterministic threshold rank in [0, 63]
 * keyed by (sx mod 8, sy mod 8). A pixel is shown iff its rank/64 <
 * smoothstep(t).
 *
 * Algorithm distinct: pixel-dissolve uses a per-pixel hash → uncorrelated
 * white-noise priority ("snow"); checker-tiles uses one priority per
 * coarse tile. Bayer is a deterministic ordered-dither field with the
 * iconic crosshatch reveal pattern — same threshold geometry, totally
 * different visual structure (regular halftone vs. random speckle).
 *
 * Pixel-art chunkiness: rank is computed in source-pixel coordinates so
 * the 3x3 dest blocks pop together. */

#include "intro_anim.h"
#include "p3a_logo.h"

/* Standard 8x8 Bayer ordered-dither matrix (values 0..63). */
static const uint8_t ia_bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
};

void ia_bayer_reveal_render(uint8_t *buffer,
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

    float s = intro_anim_smoothstep(t);
    int threshold = (int)(s * 64.0f + 0.5f);   /* 0..64 */

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            int rank = ia_bayer8[((sy & 7) << 3) | (sx & 7)];
            if (rank >= threshold) continue;

            int dx0 = ctx->logo_x + ox * scale;
            int dy0 = ctx->logo_y + oy * scale;
            for (int by = 0; by < scale; by++) {
                int ddy = dy0 + by;
                if (ddy < 0 || ddy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)ddy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int ddx = dx0 + bx;
                    if (ddx < 0 || ddx >= ctx->width) continue;
                    uint8_t *dst = row + ddx * 3;
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                }
            }
        }
    }
}
