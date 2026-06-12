// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* twirl-unwind: a Photoshop-style spiral distortion that unwinds. Each
 * source pixel is *sampled* from a position rotated about the bbox
 * center by a radius-dependent angle theta(r, t). At t=1 theta = 0
 * everywhere (canonical end state); at t=0+ theta is large at the
 * center and decays radially.
 *
 * Algorithm distinct: nonlinear vector-field warp with center-faster
 * rotation. rotozoom-settle is a uniform affine; assemble/starburst are
 * radial *translations*. This is a radial-shear *rotation* whose angle
 * varies with r.
 *
 * Per-pixel: sample at
 *     a   = (1 - smoothstep(t)) * MAX_ANGLE * exp(-r/sigma)
 *     sx' = cx + cos(a)*(sx-cx) - sin(a)*(sy-cy)
 *     sy' = cy + sin(a)*(sx-cx) + cos(a)*(sy-cy)
 * with cx, cy in source-pixel space. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_TW_MAX_ANGLE 9.0f   /* radians at center, t=0+ ≈ 1.4 turns */
#define IA_TW_SIGMA     22.0f  /* falloff in source-pixel units */

static const uint8_t *sample_or_null(const intro_anim_rot_t *r, int ox, int oy)
{
    if (ox < 0 || oy < 0 || ox >= r->rotated_w || oy >= r->rotated_h) return NULL;
    int sx = r->base_sx + r->cx_ox * ox + r->cx_oy * oy;
    int sy = r->base_sy + r->cy_ox * ox + r->cy_oy * oy;
    const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
    if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
        src[1] == P3A_LOGO_CHROMA_KEY_G &&
        src[2] == P3A_LOGO_CHROMA_KEY_R) return NULL;
    return src;
}

void ia_twirl_unwind_render(uint8_t *buffer,
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

    float cx = (float)(r.rotated_w - 1) * 0.5f;
    float cy = (float)(r.rotated_h - 1) * 0.5f;

    float k = 1.0f - intro_anim_smoothstep(t);

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            float dx = (float)ox - cx;
            float dy = (float)oy - cy;
            float rr = sqrtf(dx * dx + dy * dy);
            float angle = k * IA_TW_MAX_ANGLE * expf(-rr / IA_TW_SIGMA);
            float ca = cosf(angle);
            float sa = sinf(angle);
            float sox_f = cx + ca * dx - sa * dy;
            float soy_f = cy + sa * dx + ca * dy;
            int sox = (int)(sox_f + (sox_f >= 0.0f ? 0.5f : -0.5f));
            int soy = (int)(soy_f + (soy_f >= 0.0f ? 0.5f : -0.5f));

            const uint8_t *src = sample_or_null(&r, sox, soy);
            if (!src) continue;

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
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                }
            }
        }
    }
}
