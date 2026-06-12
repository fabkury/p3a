// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* ripple-converge: a damped 2D radial wave displaces sample positions
 * outward/inward from the bbox center. Logo pixels are sampled at a
 * radius offset by `A(t) * sin(k*r - omega*t)` along the radial; the
 * amplitude A damps to 0 by t=1 and the wave drifts outward over time.
 *
 * Algorithm distinct from wave-settle (per-row, 1D, horizontal sin) and
 * pixel-shuffle (uncorrelated 2D random offsets): here the offset is a
 * smooth radial function of (r, t) — neighboring pixels move coherently,
 * giving a water-on-a-pond look rather than horizontal corrugation or
 * speckle.
 *
 * Implementation: walk source-pixel coords (so the pixel-art block
 * identity is preserved); for each (ox, oy) compute a radial offset and
 * sample at the displaced source pixel. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_RC_AMP_SRC   7.0f   /* peak displacement, source-pixel units */
#define IA_RC_K         0.45f  /* spatial frequency */
#define IA_RC_OMEGA     8.0f   /* drift speed */

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

void ia_ripple_converge_render(uint8_t *buffer,
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

    /* Smoothstep-eased amplitude decay so the ripple hangs full and dies
     * cleanly. */
    float amp = IA_RC_AMP_SRC * (1.0f - intro_anim_smoothstep(t));
    float omega_t = IA_RC_OMEGA * t;

    float cx = (float)(r.rotated_w - 1) * 0.5f;
    float cy = (float)(r.rotated_h - 1) * 0.5f;

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            float dx = (float)ox - cx;
            float dy = (float)oy - cy;
            float rr = sqrtf(dx * dx + dy * dy);
            if (rr < 0.5f) rr = 0.5f;
            float wave = amp * sinf(IA_RC_K * rr - omega_t);
            /* Radial displacement: outward = +, inward = -. */
            float ux = dx / rr;
            float uy = dy / rr;
            int sox = ox + (int)(wave * ux + (wave * ux >= 0.0f ? 0.5f : -0.5f));
            int soy = oy + (int)(wave * uy + (wave * uy >= 0.0f ? 0.5f : -0.5f));
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
