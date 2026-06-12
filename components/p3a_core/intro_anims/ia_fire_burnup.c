// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* fire-burnup: a fiery curtain dies away to reveal the logo. The "fire"
 * is approximated by a closed-form value-noise field that drifts upward
 * with time (so flame tongues appear to rise), thresholded against a
 * descending heat budget. Where heat ≥ threshold, we paint a flame color
 * and the logo is hidden; below threshold the logo shows through.
 *
 * Algorithm distinct from any prior animation: classic demoscene fire
 * vibes via per-pixel temporal noise + threshold reveal, but kept
 * stateless (no carried buffer between frames) so the (ctx, t) contract
 * still holds bit-exactly.
 *
 * Heat field:  H(x, y, t) = bilerp of seeded value-noise on an 8-dest-px
 *              grid, with the y coordinate offset by -drift*t (sampling
 *              "future" rows = visual upward drift).
 * Threshold:   tau(t) = 1.0 - smoothstep(t) so the fire dies smoothly.
 *
 * Color: pixels where H >= tau are painted in a flame ramp keyed by
 * heat magnitude (red->orange->yellow->white). Below threshold we let
 * the canonical logo blit show. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_FB_CELL 8        /* dest-px cell size for the heat lattice */

/* 8-bit value-noise sample at lattice corner (gx, gy), driven by ctx->seed. */
static int corner_val(uint32_t seed, int gx, int gy)
{
    uint32_t h = intro_anim_hash3(seed, (uint32_t)gx, (uint32_t)gy);
    return (int)(h & 0xFFu);
}

void ia_fire_burnup_render(uint8_t *buffer,
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

    /* First paint the logo at full opacity; we'll overlay flame on top of
     * any pixel whose heat >= threshold. */
    intro_anim_fill_bg(buffer, ctx);
    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, ctx->logo_y, 255,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);

    /* Heat field & threshold. */
    int drift_dest = (int)(t * (float)ctx->height * 1.2f);   /* full screen+slack of upward drift */
    int tau_i = (int)((1.0f - intro_anim_smoothstep(t)) * 255.0f + 0.5f);
    if (tau_i <= 0) return;

    int g_w = (ctx->width  + IA_FB_CELL - 1) / IA_FB_CELL + 1;
    int g_h = (ctx->height + IA_FB_CELL - 1) / IA_FB_CELL + 2;  /* +1 for drift, +1 for safety */
    (void)g_w;
    (void)g_h;

    for (int dy = 0; dy < ctx->height; dy++) {
        int sy_dest = dy + drift_dest;          /* effective sample y, upward drift */
        int gy = sy_dest / IA_FB_CELL;
        int fy = sy_dest - gy * IA_FB_CELL;
        /* Smoothstep weights for cell interpolation. */
        float ty = (float)fy / (float)IA_FB_CELL;
        float wy = ty * ty * (3.0f - 2.0f * ty);

        uint8_t *row = buffer + (size_t)dy * ctx->stride;
        for (int dx = 0; dx < ctx->width; dx++) {
            int gx = dx / IA_FB_CELL;
            int fx = dx - gx * IA_FB_CELL;
            float tx = (float)fx / (float)IA_FB_CELL;
            float wx = tx * tx * (3.0f - 2.0f * tx);

            int v00 = corner_val(ctx->seed, gx,     gy);
            int v10 = corner_val(ctx->seed, gx + 1, gy);
            int v01 = corner_val(ctx->seed, gx,     gy + 1);
            int v11 = corner_val(ctx->seed, gx + 1, gy + 1);

            float a = (float)v00 + (float)(v10 - v00) * wx;
            float b = (float)v01 + (float)(v11 - v01) * wx;
            float h = a + (b - a) * wy;     /* in [0, 255] */

            /* Bottom-of-screen bias: heat amplifies near the bottom so
             * the flames "rise from the floor". */
            float bias = (float)dy / (float)ctx->height;   /* 0..1 top->bottom */
            float h_b = h * (0.55f + 0.45f * bias);

            int hi = (int)h_b;
            if (hi < tau_i) continue;   /* logo shows through */

            /* Flame ramp: black -> red -> orange -> yellow -> white,
             * keyed by heat above threshold. */
            int over = hi - tau_i;
            if (over > 255 - tau_i) over = 255 - tau_i;
            int range = 255 - tau_i;
            if (range < 1) range = 1;
            int k = (over * 255) / range;   /* 0..255 */

            int rr, gg, bb;
            if (k < 96) {
                /* dark red rising. */
                rr = (k * 255) / 96;
                gg = 0;
                bb = 0;
            } else if (k < 176) {
                int kk = k - 96;
                rr = 255;
                gg = (kk * 200) / 80;
                bb = 0;
            } else {
                int kk = k - 176;
                rr = 255;
                gg = 200 + (kk * 55) / 79;
                bb = (kk * 220) / 79;
            }
            row[dx * 3 + 0] = (uint8_t)bb;
            row[dx * 3 + 1] = (uint8_t)gg;
            row[dx * 3 + 2] = (uint8_t)rr;
        }
    }
}
