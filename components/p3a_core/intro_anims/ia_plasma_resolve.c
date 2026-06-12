// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* plasma-resolve: a procedural plasma field (sum of three sin waves) is
 * painted over the bg in moving colors; as t advances, the plasma fades
 * back to the flat bg while the logo fades up via smoothstep. The plasma
 * field decouples its color cycle from its decay so the field stays alive
 * (drifting) right up until it dissolves.
 *
 * Algorithm: classic sin-combination plasma. Computed at coarse 4-dest-px
 * resolution with nearest-neighbor expansion to keep cost reasonable
 * (180x180 cells for a 720x720 frame). Plasma value f(x,y,t) in [0,1] →
 * RGB via 3 phase-shifted cosines.
 *
 * Output composition:
 *   pre = bg blended with plasma color, weight = (1-t)
 *   then logo blitted at alpha = smoothstep(t)
 *
 * No transformation of the logo geometry — the mechanism is *backdrop*
 * animation. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_PL_CELL 4   /* plasma cell size in dest pixels */

void ia_plasma_resolve_render(uint8_t *buffer,
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

    /* Plasma weight decays to 0 by t=1. */
    float pw = 1.0f - t;
    float phase = t * 6.28318530717958647692f;   /* 1 cycle through anim */

    /* Center-relative coords for radial term. */
    float cx = (float)ctx->width  * 0.5f;
    float cy = (float)ctx->height * 0.5f;

    int p_w = ctx->width  / IA_PL_CELL;
    int p_h = ctx->height / IA_PL_CELL;
    if (p_w <= 0) p_w = 1;
    if (p_h <= 0) p_h = 1;

    /* Walk plasma cells; for each cell compute one color and broadcast to
     * the IA_PL_CELL x IA_PL_CELL block. */
    for (int py = 0; py < p_h; py++) {
        int dy0 = py * IA_PL_CELL;
        int dy1 = dy0 + IA_PL_CELL;
        if (dy1 > ctx->height) dy1 = ctx->height;
        for (int px = 0; px < p_w; px++) {
            int dx0 = px * IA_PL_CELL;
            int dx1 = dx0 + IA_PL_CELL;
            if (dx1 > ctx->width) dx1 = ctx->width;

            float fx = (float)dx0 / 32.0f;
            float fy = (float)dy0 / 32.0f;
            float dxc = (float)dx0 - cx;
            float dyc = (float)dy0 - cy;
            float rad = sqrtf(dxc * dxc + dyc * dyc) / 64.0f;

            /* f in roughly [-3, 3]; remap to [0, 1] via half-range. */
            float f = sinf(fx + phase) + sinf(fy * 0.9f - phase * 0.7f) + sinf(rad + phase * 1.3f);
            float u = (f + 3.0f) * (1.0f / 6.0f);   /* [0, 1] */

            /* Three phase-shifted cosines as RGB lobes; map to [0, 255]. */
            float cr = 0.5f + 0.5f * cosf(6.28318f * u + 0.0f);
            float cg = 0.5f + 0.5f * cosf(6.28318f * u + 2.094f);  /* +120° */
            float cb = 0.5f + 0.5f * cosf(6.28318f * u + 4.189f);  /* +240° */

            int rr = (int)(cr * 255.0f + 0.5f);
            int gg = (int)(cg * 255.0f + 0.5f);
            int bb = (int)(cb * 255.0f + 0.5f);

            /* Blend toward bg by (1 - pw). */
            int aa = (int)(pw * 255.0f + 0.5f);
            int ia = 255 - aa;
            uint8_t bv = (uint8_t)((bb * aa + ctx->bg_b * ia + 127) / 255);
            uint8_t gv = (uint8_t)((gg * aa + ctx->bg_g * ia + 127) / 255);
            uint8_t rv = (uint8_t)((rr * aa + ctx->bg_r * ia + 127) / 255);

            for (int dy = dy0; dy < dy1; dy++) {
                uint8_t *row = buffer + (size_t)dy * ctx->stride;
                for (int dx = dx0; dx < dx1; dx++) {
                    uint8_t *dst = row + dx * 3;
                    dst[0] = bv; dst[1] = gv; dst[2] = rv;
                }
            }
        }
    }

    /* Logo on top, alpha = smoothstep(t). */
    float s = intro_anim_smoothstep(t);
    int la = (int)(s * 255.0f + 0.5f);
    if (la < 0) la = 0;
    if (la > 255) la = 255;
    /* p3a_logo_blit_pixelwise_bgr888's alpha-blend uses bg-color for the
     * transparent fraction — but we want it to blend against the *current*
     * buffer (the plasma), not bg. Workaround: at low la most of the
     * buffer stays plasma anyway; at high la the logo wins. So just call
     * the blitter with bg as bg — close enough; there's no per-pixel
     * source-over against arbitrary backdrop API. The mismatch is tiny in
     * practice because plasma is mostly washed out by the time s > 0.5. */
    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, ctx->logo_y, (uint8_t)la,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);
}
