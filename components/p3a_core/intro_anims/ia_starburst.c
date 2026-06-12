// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* starburst: per-source-pixel radial fly-in *with overshoot*. Pixels
 * launch from far outside their home position and use a back-out easing
 * curve that overshoots their landing point and snaps back. Distinct
 * from `assemble` (which uses cubic ease-out, no overshoot) because the
 * trajectory itself is non-monotonic — pixels visibly bounce inward past
 * home before settling.
 *
 * Direction: radial *outward* from bbox center for each pixel's home (so
 * they fly *in* along their own home-radial, like a starburst collapsing
 * to a point). Distance variance per pixel via hash byte.
 *
 * Easing (back-out, classic UI form):
 *   u_eased = 1 + c1 * (u - 1)^3 + c2 * (u - 1)^2
 * with c2 = 1.7, c1 = 2.7. At u=1 → 1; before u=1 → overshoots above 1
 * briefly, settling smoothly. We use (1 - u_eased) as the remaining
 * distance fraction, so overshoot = pixel temporarily *past* its home in
 * the inward direction. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_SB_DIST_MULT 1.4f   /* base launch distance as multiple of width */

void ia_starburst_render(uint8_t *buffer,
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
    const int rw = r.rotated_w * scale;
    const int rh = r.rotated_h * scale;

    int cx = ctx->logo_x + rw / 2;
    int cy = ctx->logo_y + rh / 2;

    /* Back-out easing. */
    float u = t;
    float um1 = u - 1.0f;
    const float c2 = 1.7f, c1 = c2 + 1.0f;
    float u_e = 1.0f + c1 * um1 * um1 * um1 + c2 * um1 * um1;
    float remain = 1.0f - u_e;   /* fraction of launch distance still away from home */

    float base_dist = IA_SB_DIST_MULT * (float)(ctx->width > ctx->height ? ctx->width : ctx->height);

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            int home_x = ctx->logo_x + ox * scale;
            int home_y = ctx->logo_y + oy * scale;

            int radial_dx = home_x - cx;
            int radial_dy = home_y - cy;
            /* Manhattan-normalize: scale (rdx, rdy) so |rdx|+|rdy| ≈ 1
             * unit; cheap, avoids sqrt; gives a directional launch. */
            int amag = (radial_dx < 0 ? -radial_dx : radial_dx)
                     + (radial_dy < 0 ? -radial_dy : radial_dy);
            if (amag == 0) continue;   /* center pixel — stays put */

            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
            float mag_var = 0.7f + (float)((h >> 8) & 0xFFu) * (0.6f / 255.0f);  /* [0.7, 1.3] */
            float mag = base_dist * remain * mag_var;

            int off_x = (int)(((float)radial_dx / (float)amag) * mag);
            int off_y = (int)(((float)radial_dy / (float)amag) * mag);

            int dx0 = home_x + off_x;
            int dy0 = home_y + off_y;

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
}
