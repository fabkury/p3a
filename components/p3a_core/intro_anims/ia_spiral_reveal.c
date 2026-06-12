// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* spiral-reveal: a rotating cone of visibility sweeps clockwise around the
 * bbox center while a radial threshold expands outward. A pixel is shown
 * once both:
 *   (a) its angular distance from the sweep ray is within a half-aperture
 *       that grows with t (so by t=1 the aperture covers the full circle),
 *   (b) its radius from center is within an expanding radial threshold.
 *
 * The combination gives a "spiral arm" reveal: pixels closer to center
 * appear first, distant pixels wait their turn, and within any radius the
 * angular sweep order is preserved.
 *
 * t=1 ends with full aperture and full radius — every opaque pixel shown,
 * matching the canonical end state. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

void ia_spiral_reveal_render(uint8_t *buffer,
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

    const float TWOPI = 6.28318530717958647692f;
    float cx = (float)(r.rotated_w - 1) * 0.5f;
    float cy = (float)(r.rotated_h - 1) * 0.5f;

    /* Max radius from center to a corner of the source rect. */
    float max_r = sqrtf(cx * cx + cy * cy);

    /* Sweep ray angle: 1.5 turns over t (gives a meaningful spiral arm). */
    float sweep_a = TWOPI * 1.5f * t;

    /* Aperture grows from a thin slice (~30°) to full circle (PI). */
    float aperture = 0.26f + (3.14159265358979323846f - 0.26f) * t;

    /* Radial threshold: expands smoothstep-paced to (max_r + slack). */
    float s = intro_anim_smoothstep(t);
    float r_max = max_r * (s * 1.05f);

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            float dxa = (float)ox - cx;
            float dya = (float)oy - cy;
            float rad = sqrtf(dxa * dxa + dya * dya);
            if (rad > r_max) continue;

            float a = atan2f(dya, dxa);   /* (-PI, PI] */
            /* Angular distance from sweep ray, wrapped to [0, PI]. */
            float d = a - sweep_a;
            d = fmodf(d, TWOPI);
            if (d < 0.0f) d += TWOPI;
            if (d > 3.14159265358979323846f) d = TWOPI - d;
            if (d > aperture) continue;

            int dx0 = ctx->logo_x + ox * scale;
            int dy0 = ctx->logo_y + oy * scale;
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
