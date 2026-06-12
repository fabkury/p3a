// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* rotate-step: snap through 4 quarter-turns toward ctx->rotation while
 * scaling up from 1 to ctx->logo_scale. Each step lasts 0.25 of t and
 * holds its rotation/scale (no interpolation — pixel-art-safe).
 *
 * Steps (with target rotation R = ctx->rotation):
 *   t in [0.00, 0.25): rot = (R + 270) % 360, scale = 1
 *   t in [0.25, 0.50): rot = (R + 180) % 360, scale = lerp partial
 *   t in [0.50, 0.75): rot = (R +  90) % 360, scale = lerp partial
 *   t in [0.75, 1.00): rot =  R,              scale = ctx->logo_scale
 *   t = 1                                 -> canonical blit (early return)
 *
 * Center of the rendered image stays pinned to the final centered logo's
 * center across all steps, so the snaps look like the same icon spinning
 * in place rather than jumping around. */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_rotate_step_render(uint8_t *buffer,
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

    /* Final-image bbox center: we use this as the target screen point that
     * each intermediate snap centers itself on. */
    int final_rw_src = (ctx->rotation == 90 || ctx->rotation == 270) ? p3a_logo_h : p3a_logo_w;
    int final_rh_src = (ctx->rotation == 90 || ctx->rotation == 270) ? p3a_logo_w : p3a_logo_h;
    int center_x = ctx->logo_x + (final_rw_src * ctx->logo_scale) / 2;
    int center_y = ctx->logo_y + (final_rh_src * ctx->logo_scale) / 2;

    int step = (int)(t * 4.0f);
    if (step < 0) step = 0;
    if (step > 3) step = 3;

    /* Quarter-turn offsets in cw degrees, 270 -> 180 -> 90 -> 0. */
    static const int rot_offsets[4] = { 270, 180, 90, 0 };
    int rot_step = (ctx->rotation + rot_offsets[step]) % 360;

    /* Scale ramp: 1 at step 0, ctx->logo_scale at step 3, linear interp
     * for the middle two; clamped to 1. */
    int scale_step;
    if (step == 0) {
        scale_step = 1;
    } else if (step == 3) {
        scale_step = ctx->logo_scale;
    } else {
        /* step 1 -> 1/3, step 2 -> 2/3 of the way from 1 to logo_scale. */
        int delta = ctx->logo_scale - 1;
        scale_step = 1 + (delta * step + 1) / 3;   /* +1 for round-half-up */
        if (scale_step < 1) scale_step = 1;
    }

    /* Compute step's rotated source dims and centered top-left. */
    int rw_src = (rot_step == 90 || rot_step == 270) ? p3a_logo_h : p3a_logo_w;
    int rh_src = (rot_step == 90 || rot_step == 270) ? p3a_logo_w : p3a_logo_h;
    int tl_x = center_x - (rw_src * scale_step) / 2;
    int tl_y = center_y - (rh_src * scale_step) / 2;

    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        tl_x, tl_y, 255,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        scale_step, rot_step);
}
