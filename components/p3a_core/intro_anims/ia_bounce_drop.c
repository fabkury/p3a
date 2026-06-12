// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* bounce-drop: the whole logo drops from above the screen and bounces twice
 * with damped amplitude before settling at its centered position. Render is
 * pure (no integration), but the y-offset traces a piecewise-quadratic
 * trajectory that reads as physical without floating bookkeeping.
 *
 * Bounce model (closed-form, monotonic in t):
 *   - Three "drop" arcs back-to-back; arc i runs over t in [Ti, Ti+1].
 *   - Each arc is a falling parabola: starts at height H_i, lands at 0 at
 *     end of arc.
 *   - Heights H_0 = drop_height, H_1 = 0.18*H_0, H_2 = 0.06*H_0.
 *   - Arc durations weight earlier arcs longer to feel like real damping.
 *   - At t=1 we land exactly at offset 0 (canonical end-state).
 *
 * Y-only motion → can use the canonical blitter once per frame with a
 * shifted dest position (no per-pixel work here). */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_bounce_drop_render(uint8_t *buffer,
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

    /* Drop height: enough to start fully off-screen above. logo_y can be
     * positive (centered on tall canvas), so drop from -H so that the logo
     * top is offscreen at t=0+. */
    const float drop_h = (float)(ctx->logo_y + ctx->height / 2);

    /* Arc time boundaries in t: T0=0, T1=0.55, T2=0.85, T3=1.0.
     * Arc heights: H0=drop_h, H1=0.18*drop_h, H2=0.06*drop_h. */
    static const float T1 = 0.55f, T2 = 0.85f;
    float h0 = drop_h, h1 = 0.18f * drop_h, h2 = 0.06f * drop_h;

    float y_off;   /* offset above settled position; 0 = centered */
    if (t < T1) {
        /* Falling arc 0: parabola y = H0*(1-u)^2, where u = t/T1, so y(0)=H0,
         * y(T1)=0. Quadratic gives the "speeds up as it falls" feel. */
        float u = t / T1;
        float v = 1.0f - u;
        y_off = h0 * v * v;
    } else if (t < T2) {
        /* Up-then-down: bounce 1. Symmetric arc peaking at midpoint with
         * height h1, value 0 at both ends. y = h1 * 4 * u * (1 - u). */
        float u = (t - T1) / (T2 - T1);
        y_off = h1 * 4.0f * u * (1.0f - u);
    } else {
        /* Bounce 2, smaller, ending exactly at 0 by t=1. */
        float u = (t - T2) / (1.0f - T2);
        y_off = h2 * 4.0f * u * (1.0f - u);
    }

    /* Negative offset → above settled position. */
    int dy_logo = ctx->logo_y - (int)(y_off + 0.5f);

    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, dy_logo, 255,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);
}
