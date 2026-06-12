// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* rotozoom-settle: classic affine warp that decays toward identity. The
 * logo starts oversized and rotated several turns and settles to its
 * final orientation/size by t=1. Affine matrix combines uniform scale
 * s(t) with rotation theta(t), both eased smoothstep.
 *
 * Algorithm distinct: 2D affine inverse-mapping in source coordinates.
 * No prior animation does a uniform rotation+scale of the whole image
 * — the closest neighbors (twirl-unwind, assemble) are radial-shear or
 * radial-translation respectively, not rigid affine.
 *
 * The warp is applied at the source-pixel level (so the pixel-art block
 * structure is preserved): for each (ox, oy), back-transform to the
 * logo's local coords by the inverse rotation+scale and sample. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_RZ_TURNS  1.5f   /* total rotation traversed at t=0 (clockwise) */
#define IA_RZ_SCALE  3.5f   /* peak scale at t=0+; settles to 1 at t=1 */

void ia_rotozoom_settle_render(uint8_t *buffer,
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

    /* Eased remaining-distance to identity. */
    float k = 1.0f - intro_anim_smoothstep(t);
    float theta = IA_RZ_TURNS * 6.28318530717958647692f * k;
    float s_zoom = 1.0f + (IA_RZ_SCALE - 1.0f) * k;
    float inv_s = 1.0f / s_zoom;

    /* Sampling matrix (inverse rotation + inverse scale) applied about
     * the logo bbox center in dest space. Dest -> rotated-source (ox,oy)
     * via:
     *   px = (dx - cx)*inv_s
     *   py = (dy - cy)*inv_s
     *   sx = ca * px + sa * py + rotated_w/2
     *   sy = -sa * px + ca * py + rotated_h/2
     */
    float ca = cosf(theta);
    float sa = sinf(theta);
    float cdx = (float)(ctx->logo_x + rw / 2);
    float cdy = (float)(ctx->logo_y + rh / 2);
    float src_cx = (float)(r.rotated_w - 1) * 0.5f;
    float src_cy = (float)(r.rotated_h - 1) * 0.5f;

    /* Destination bbox to walk: scaled bbox, clipped to screen. */
    int span_x = (int)((float)rw * 0.5f * s_zoom + 1.0f);
    int span_y = (int)((float)rh * 0.5f * s_zoom + 1.0f);
    int x0 = (int)cdx - span_x;
    int y0 = (int)cdy - span_y;
    int x1 = (int)cdx + span_x;
    int y1 = (int)cdy + span_y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ctx->width)  x1 = ctx->width;
    if (y1 > ctx->height) y1 = ctx->height;

    for (int dy = y0; dy < y1; dy++) {
        float py = ((float)dy - cdy) * inv_s;
        uint8_t *row = buffer + (size_t)dy * ctx->stride;
        for (int dx = x0; dx < x1; dx++) {
            float px = ((float)dx - cdx) * inv_s;
            /* Map to rotated-source (ox, oy) in source-pixel units. */
            float sox_f = (ca * px + sa * py) / (float)scale + src_cx;
            float soy_f = (-sa * px + ca * py) / (float)scale + src_cy;
            int ox = (int)(sox_f + (sox_f >= 0.0f ? 0.5f : -0.5f));
            int oy = (int)(soy_f + (soy_f >= 0.0f ? 0.5f : -0.5f));
            if (ox < 0 || oy < 0 || ox >= r.rotated_w || oy >= r.rotated_h) continue;
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
