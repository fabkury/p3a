// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* diamond-wipe: an L1 (Manhattan) reveal from bbox center. Visible region
 * grows as { (dx, dy) | |dx| + |dy| <= r(t) }, which renders as a rotated
 * square (a diamond). Different metric from iris-wipe (L2) so the visual
 * mechanism is genuinely distinct: corners of the diamond reach the bbox
 * edges *first*, in the four cardinal directions, then sides catch up.
 *
 * Pacing: smoothstep on r(t). Max distance is dx_half + dy_half so the
 * diamond can fully cover the bbox by t=1. */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_diamond_wipe_render(uint8_t *buffer,
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

    int dx_half = (rw + 1) / 2;
    int dy_half = (rh + 1) / 2;
    int max_d = dx_half + dy_half;

    float s = intro_anim_smoothstep(t);
    int rd = (int)((float)max_d * s + 0.5f);

    int x0 = ctx->logo_x, y0 = ctx->logo_y;
    int x1 = x0 + rw,     y1 = y0 + rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ctx->width)  x1 = ctx->width;
    if (y1 > ctx->height) y1 = ctx->height;

    for (int dy = y0; dy < y1; dy++) {
        uint8_t *row = buffer + (size_t)dy * ctx->stride;
        int ady = dy - cy;
        if (ady < 0) ady = -ady;
        int budget = rd - ady;
        if (budget < 0) continue;
        int xlo = cx - budget;
        int xhi = cx + budget;
        if (xlo < x0) xlo = x0;
        if (xhi >= x1) xhi = x1 - 1;
        for (int dx = xlo; dx <= xhi; dx++) {
            int ox = (dx - ctx->logo_x) / scale;
            int oy = (dy - ctx->logo_y) / scale;
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;
            uint8_t *dst = row + dx * 3;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        }
    }
}
