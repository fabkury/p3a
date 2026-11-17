// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* mosaic-shrink: render the logo at a coarse pixelation (block of B src px
 * = B*scale dest px collapses to one color, taken from the block's
 * top-left source sample), with B shrinking from 12 to 1 over t. The
 * coarsest blocks at t=0+ look like a chunky mosaic; by t=1 we're at
 * source resolution and delegate to the canonical blitter.
 *
 * Algorithm: an explicit per-block iteration (not a per-dest-pixel one),
 * so the inner cost scales with (block_count * block_area) ≈ rotated_area
 * regardless of B. Alpha fade via smoothstep makes the transition smooth
 * even when B drops by a step. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_MS_BLOCK_MAX 12

void ia_mosaic_shrink_render(uint8_t *buffer,
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

    /* Block size in source pixels: integer step from MAX..1.
     * t=0+: B=MAX; t->1: B=1. */
    int B = IA_MS_BLOCK_MAX - (int)(t * (float)(IA_MS_BLOCK_MAX - 1) + 0.5f);
    if (B < 1) B = 1;
    if (B > IA_MS_BLOCK_MAX) B = IA_MS_BLOCK_MAX;

    /* Alpha fade lifts the bg through the mosaic so it doesn't feel like a
     * hard pop. */
    float s = intro_anim_smoothstep(t);
    int a = (int)(s * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    int ia = 255 - a;

    for (int by = 0; by < r.rotated_h; by += B) {
        for (int bx = 0; bx < r.rotated_w; bx += B) {
            /* Block sample: source pixel at the block's top-left in
             * rotated coords. If transparent, we skip the block — gives
             * a cleaner silhouette than averaging. */
            int sx = r.base_sx + r.cx_ox * bx + r.cx_oy * by;
            int sy = r.base_sy + r.cy_ox * bx + r.cy_oy * by;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            /* Block coverage in dest space. */
            int dx0 = ctx->logo_x + bx * scale;
            int dy0 = ctx->logo_y + by * scale;
            int dx1 = dx0 + B * scale;
            int dy1 = dy0 + B * scale;
            /* Don't run past the rotated bbox. */
            int max_dx = ctx->logo_x + r.rotated_w * scale;
            int max_dy = ctx->logo_y + r.rotated_h * scale;
            if (dx1 > max_dx) dx1 = max_dx;
            if (dy1 > max_dy) dy1 = max_dy;

            /* Per-channel block fill with alpha-blend toward bg. */
            uint8_t bv = (uint8_t)((src[0] * a + ctx->bg_b * ia + 127) / 255);
            uint8_t gv = (uint8_t)((src[1] * a + ctx->bg_g * ia + 127) / 255);
            uint8_t rv = (uint8_t)((src[2] * a + ctx->bg_r * ia + 127) / 255);

            for (int dy = dy0; dy < dy1; dy++) {
                if (dy < 0 || dy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)dy * ctx->stride;
                for (int dx = dx0; dx < dx1; dx++) {
                    if (dx < 0 || dx >= ctx->width) continue;
                    uint8_t *dst = row + dx * 3;
                    dst[0] = bv; dst[1] = gv; dst[2] = rv;
                }
            }
        }
    }
}
