// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* pixel-rain: each opaque source pixel falls from above its home y to
 * its home position. Three independent seeded hashes per pixel give a
 * heterogenous-rain look rather than a uniform cloud:
 *
 *   - start delay  s0 ∈ [0, 0.55)    — wide stagger so pixels enter
 *                                     across more than half the window
 *   - drop distance dist ∈ [0.55, 1.7] × screen — varies pixel landing
 *                                     speed AND visible offset at t=0+
 *   - ease shape   p ∈ {4, 5, 6, 7}  — different "remaining distance"
 *                                     curves, so trajectories don't
 *                                     all collapse to the same shape
 *
 * Result: pixels arrive over a much wider spread, with visibly different
 * speeds, and the cohort no longer reads as a single dense cloud. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_PR_STAGGER     0.55f    /* max per-pixel start time */
#define IA_PR_DIST_MIN    0.55f    /* min drop distance, multiple of screen */
#define IA_PR_DIST_RANGE  1.15f    /* max - min */

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

    const float screen_h = (float)(ctx->logo_y + ctx->height);

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
            /* Pull three independent variability bytes from one hash. */
            uint32_t b_start = h & 0xFFu;
            uint32_t b_dist  = (h >> 8) & 0xFFu;
            uint32_t b_ease  = (h >> 16) & 0x3u;   /* 0..3 → power 4..7 */

            float s0 = (float)b_start * (IA_PR_STAGGER / 255.0f);
            if (t < s0) continue;

            float u = (t - s0) / (1.0f - s0);
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;

            /* Per-pixel ease shape: (1-u)^p with p in {4,5,6,7}, scaled
             * by per-pixel drop distance. Higher p → drop happens later
             * and faster; lower p → smoother glide. */
            float v = 1.0f - u;
            float v_pow = v * v * v * v;          /* (1-u)^4 */
            int extra = (int)b_ease;              /* 0..3 */
            for (int k = 0; k < extra; k++) v_pow *= v;   /* up to (1-u)^7 */

            float dist = (IA_PR_DIST_MIN + (float)b_dist * (IA_PR_DIST_RANGE / 255.0f)) * screen_h;
            int off_y = (int)(v_pow * dist + 0.5f);

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
