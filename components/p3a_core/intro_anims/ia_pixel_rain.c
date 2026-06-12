// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* pixel-rain: each opaque source pixel falls from above its home y to its
 * home position. Per-pixel start delay drawn from a seeded hash so they
 * don't all leave at once; per-pixel fall window has cubic ease-out so
 * pixels accelerate as they fall and slow as they land.
 *
 * Stagger window: each pixel has personal start s0 in [0, 0.35] and ends
 * at 1.0; pixels with later starts have shorter, faster falls. This gives
 * a layered "rain ending in a thud" feel rather than a synchronous drop. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_PR_STAGGER 0.35f      /* max per-pixel start time */

void ia_pixel_rain_render(uint8_t *buffer,
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

    /* Drop distance: fall starts from above the screen so any pixel still
     * en route at t=0+ is visually offscreen. */
    const float drop_dist = (float)(ctx->logo_y + ctx->height);

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
            float s0 = (float)(h & 0xFFFFu) * (IA_PR_STAGGER / 65535.0f);
            if (t < s0) continue;   /* still in start delay = above screen */

            /* Local fall progress in [0, 1] over [s0, 1]. */
            float u = (t - s0) / (1.0f - s0);
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;

            /* Cubic ease-out: cover ground fast, settle slow. */
            float v = 1.0f - u;
            float ease = 1.0f - v * v * v;

            float drop = (1.0f - ease) * drop_dist;
            int off_y = (int)(drop + 0.5f);

            int dx0 = ctx->logo_x + ox * scale;
            int dy0 = ctx->logo_y + oy * scale - off_y;

            for (int by = 0; by < scale; by++) {
                int dy = dy0 + by;
                if (dy < 0 || dy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)dy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int dx = dx0 + bx;
                    if (dx < 0 || dx >= ctx->width) continue;
                    uint8_t *dst = row + dx * 3;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
        }
    }
}
