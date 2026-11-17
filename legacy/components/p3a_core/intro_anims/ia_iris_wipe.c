// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* iris-wipe: a circular reveal that grows from the logo center to cover the
 * full bounding box. Inside the iris, the logo is drawn opaque; outside,
 * background. At t=1 the iris fully covers the bbox (delegated to the
 * canonical blitter for exactness). */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_iris_wipe_render(uint8_t *buffer,
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

    /* Iris center = bbox center, shifted 10 source pixels (= 10 * scale dest
     * pixels) toward screen-up. Focuses the wipe on the upper part of the
     * logo where the main p3a glyph content lives. */
    const int center_offset_dy = -10 * scale;
    const int cx = ctx->logo_x + rw / 2;
    const int cy = ctx->logo_y + rh / 2 + center_offset_dy;

    /* Max radius squared: pick the farther of (top corners, bottom corners).
     * Bottom is farther whenever center_offset_dy < 0, but compute it
     * generically so a future positive offset Just Works. */
    const int dx_half  = (rw + 1) / 2;
    const int d_top    = cy - ctx->logo_y;
    const int d_bottom = (ctx->logo_y + rh) - cy;
    const int dy_max   = (d_top > d_bottom) ? d_top : d_bottom;
    const int max_r2   = dx_half * dx_half + dy_max * dy_max;

    float s = intro_anim_smoothstep(t);
    int r2 = (int)((float)max_r2 * s + 0.5f);

    int x0 = ctx->logo_x, y0 = ctx->logo_y;
    int x1 = x0 + rw,     y1 = y0 + rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ctx->width)  x1 = ctx->width;
    if (y1 > ctx->height) y1 = ctx->height;

    for (int dy = y0; dy < y1; dy++) {
        uint8_t *row = buffer + (size_t)dy * ctx->stride;
        int ddy = dy - cy;
        int ddy2 = ddy * ddy;
        for (int dx = x0; dx < x1; dx++) {
            int ddx = dx - cx;
            if (ddx * ddx + ddy2 > r2) continue;

            int ox = (dx - ctx->logo_x) / scale;
            int oy = (dy - ctx->logo_y) / scale;
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            uint8_t *dst = row + dx * 3;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
    }
}
