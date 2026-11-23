// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* scanline-reveal: a CRT-style bright bar sweeps top->bottom across the logo
 * bounding box (in screen space, so it stays natural under any rotation).
 * Above the bar's leading edge: opaque logo. At the bar: a contrasting
 * bright stripe. Below the bar: still background.
 *
 * Bar contrast color is chosen at runtime against bg so the effect reads on
 * both light and dark themes. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_SR_BAR_THICKNESS 6   /* dest pixels */

void ia_scanline_reveal_render(uint8_t *buffer,
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

    /* Bar contrast: invert against bg luminance so the sweep is visible on
     * any theme. Bright bg -> dark bar; dark bg -> light bar. */
    int lum = (int)ctx->bg_r * 299 + (int)ctx->bg_g * 587 + (int)ctx->bg_b * 114;
    /* lum range: 0..255*1000 = 0..255000; midpoint 127500. */
    uint8_t bar_v = (lum < 127500) ? 240 : 24;

    /* Bar leading edge sweeps logo_y..logo_y+rh as t goes 0..1. */
    int y_lead = ctx->logo_y + (int)((float)rh * t + 0.5f);
    int y_bar0 = y_lead - IA_SR_BAR_THICKNESS / 2;
    int y_bar1 = y_lead + (IA_SR_BAR_THICKNESS - IA_SR_BAR_THICKNESS / 2);

    int y0 = ctx->logo_y, y1 = ctx->logo_y + rh;
    int x0 = ctx->logo_x, x1 = ctx->logo_x + rw;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > ctx->height) y1 = ctx->height;
    if (x1 > ctx->width)  x1 = ctx->width;

    for (int dy = y0; dy < y1; dy++) {
        uint8_t *row = buffer + (size_t)dy * ctx->stride;
        int in_bar    = (dy >= y_bar0 && dy < y_bar1);
        int above_bar = (dy < y_bar0);
        if (!in_bar && !above_bar) continue;   /* below bar = bg, already filled */

        for (int dx = x0; dx < x1; dx++) {
            int ox = (dx - ctx->logo_x) / scale;
            int oy = (dy - ctx->logo_y) / scale;
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            int is_logo = !(src[0] == P3A_LOGO_CHROMA_KEY_B &&
                            src[1] == P3A_LOGO_CHROMA_KEY_G &&
                            src[2] == P3A_LOGO_CHROMA_KEY_R);

            uint8_t *dst = row + dx * 3;
            if (in_bar) {
                /* Bar dominates inside the bbox, regardless of logo
                 * transparency — gives a clean swept-stripe look. */
                dst[0] = bar_v;
                dst[1] = bar_v;
                dst[2] = bar_v;
            } else if (is_logo) {
                /* above_bar */
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            /* else above_bar but transparent source: leave bg as-is. */
        }
    }
}
