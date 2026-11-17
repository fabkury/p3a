// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* swirl-in: each opaque source pixel spirals in toward its home
 * position along a curved polar arc — its radius from the logo center
 * shrinks while its angle unwinds, so the logo gathers like a little
 * galaxy. Per-pixel hash varies the launch radius so the cloud has
 * depth; the swirl direction flips per boot (seed bit).
 *
 * Algorithm distinct from assemble/starburst (straight-line radial
 * fly-ins): paths here are genuinely curved, computed in polar space
 * every frame. Displacement is expressed relative to the recomputed
 * home polar position, so at t=1 the offset collapses to exactly 0. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_SW_TWO_PI      6.28318530717958647692f
#define IA_SW_EXTRA_R     0.75f   /* launch radius bonus, x screen width */
#define IA_SW_TURNS       1.1f    /* angle unwound across the window */

void ia_swirl_in_render(uint8_t *buffer,
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

    /* Cubic ease-out; decay = remaining fraction of the journey. */
    float inv = 1.0f - t;
    float decay = inv * inv * inv;

    float lcx = (float)ctx->logo_x + (float)(r.rotated_w * scale) * 0.5f;
    float lcy = (float)ctx->logo_y + (float)(r.rotated_h * scale) * 0.5f;
    float dir = ((ctx->seed >> 4) & 1u) ? 1.0f : -1.0f;
    float swirl = dir * IA_SW_TURNS * IA_SW_TWO_PI * decay;
    float extra_base = (float)ctx->width * IA_SW_EXTRA_R * decay;

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            /* Home pixel center, relative to the logo center. */
            float hx = (float)(ctx->logo_x + ox * scale) + (float)scale * 0.5f - lcx;
            float hy = (float)(ctx->logo_y + oy * scale) + (float)scale * 0.5f - lcy;
            float r0 = sqrtf(hx * hx + hy * hy);
            float th0 = atan2f(hy, hx);

            /* Per-pixel launch-radius variance in [0.7, 1.3]. */
            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
            float mag_var = 0.7f + (float)(h & 0xFFu) * (0.6f / 255.0f);

            float cur_r = r0 + extra_base * mag_var;
            float cur_th = th0 + swirl;

            /* Offset relative to the recomputed home position so the
             * polar->cartesian roundtrip cancels exactly as decay->0. */
            float off_x = cur_r * cosf(cur_th) - hx;
            float off_y = cur_r * sinf(cur_th) - hy;

            int dx0 = ctx->logo_x + ox * scale + (int)floorf(off_x + 0.5f);
            int dy0 = ctx->logo_y + oy * scale + (int)floorf(off_y + 0.5f);

            for (int by = 0; by < scale; by++) {
                int dy = dy0 + by;
                if (dy < 0 || dy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)dy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int dx = dx0 + bx;
                    if (dx < 0 || dx >= ctx->width) continue;
                    uint8_t *dst = row + dx * 3;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
        }
    }
}
