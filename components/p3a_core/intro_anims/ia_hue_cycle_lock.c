// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* hue-cycle-lock: render the logo at full alpha but with its colors
 * rotated through HSV-hue space; the hue offset decays from a full
 * spectrum sweep at t=0+ down to 0 at t=1 (true colors).
 *
 * Algorithm distinct: pure HSV color-space rotation. color-emerge does
 * an RGB lerp between two fixed colors; this rotates the hue *of every
 * pixel's own color* through the color circle. No motion, no masking,
 * no opacity change.
 *
 * RGB→HSV→rotate→HSV→RGB inline (no LUT, no library). To stay cheap on
 * the embedded target we use 6-segment hue arithmetic that avoids any
 * trig. */

#include "intro_anim.h"
#include "p3a_logo.h"

/* Returns hue scaled to [0, 1530) (6 * 255), saturation [0, 255], value
 * [0, 255]. Working in 1530 lets us hue-rotate by integer addition
 * without drift. */
static void rgb_to_hsv1530(uint8_t r, uint8_t g, uint8_t b,
                           int *out_h, int *out_s, int *out_v)
{
    int rmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int rmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int delta = rmax - rmin;
    *out_v = rmax;
    *out_s = (rmax > 0) ? (delta * 255) / rmax : 0;
    if (delta == 0) {
        *out_h = 0;
        return;
    }
    int h;
    if (rmax == r) {
        h = 0 * 255 + ((g - b) * 255) / delta;
    } else if (rmax == g) {
        h = 2 * 255 + ((b - r) * 255) / delta;
    } else {
        h = 4 * 255 + ((r - g) * 255) / delta;
    }
    if (h < 0) h += 1530;
    if (h >= 1530) h -= 1530;
    *out_h = h;
}

static void hsv1530_to_rgb(int h, int s, int v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = (uint8_t)v; return; }
    if (h < 0) h += 1530;
    if (h >= 1530) h -= 1530;
    int seg = h / 255;        /* 0..5 */
    int f   = h - seg * 255;  /* 0..254 */
    int p = (v * (255 - s)) / 255;
    int q = (v * (255 - (s * f) / 255)) / 255;
    int u = (v * (255 - (s * (255 - f)) / 255)) / 255;
    int rr, gg, bb;
    switch (seg) {
        case 0: rr = v; gg = u; bb = p; break;
        case 1: rr = q; gg = v; bb = p; break;
        case 2: rr = p; gg = v; bb = u; break;
        case 3: rr = p; gg = q; bb = v; break;
        case 4: rr = u; gg = p; bb = v; break;
        default:rr = v; gg = p; bb = q; break;
    }
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
}

void ia_hue_cycle_lock_render(uint8_t *buffer,
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

    /* Hue offset: at t=0+ swing through a full 720° (two cycles) of hue
     * rotation; decays smoothstep-paced to 0 at t=1. */
    float k = 1.0f - intro_anim_smoothstep(t);
    int hue_off = (int)(k * 2.0f * 1530.0f) % 1530;

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            /* src is BGR888. */
            int hh, ss, vv;
            rgb_to_hsv1530(src[2], src[1], src[0], &hh, &ss, &vv);
            hh += hue_off;
            if (hh >= 1530) hh -= 1530;
            uint8_t out_r, out_g, out_b;
            hsv1530_to_rgb(hh, ss, vv, &out_r, &out_g, &out_b);

            int dx0 = ctx->logo_x + ox * scale;
            int dy0 = ctx->logo_y + oy * scale;
            for (int by = 0; by < scale; by++) {
                int ddy = dy0 + by;
                if (ddy < 0 || ddy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)ddy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int ddx = dx0 + bx;
                    if (ddx < 0 || ddx >= ctx->width) continue;
                    uint8_t *dst = row + ddx * 3;
                    dst[0] = out_b;
                    dst[1] = out_g;
                    dst[2] = out_r;
                }
            }
        }
    }
}
