// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* plasma-dissolve: same dissolve geometry as pixel-dissolve, but the
 * threshold field is a smooth plasma (sum of three sines) instead of a
 * per-pixel hash. The reveal looks like organic blobs growing into each
 * other rather than random snow.
 *
 * Algorithm distinct: pixel-dissolve sorts hashes (uncorrelated white
 * noise → speckle); plasma-dissolve uses a continuous correlated field
 * (low-frequency, smooth → blobs). Same per-source-pixel chunkiness:
 * one threshold per source pixel preserves the 3x3 block identity.
 *
 * Threshold field f(sx, sy) is normalized to [0, 1] once at frame setup
 * by knowing sin's bounds; then visible iff f(sx, sy) <= smoothstep(t). */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

void ia_plasma_dissolve_render(uint8_t *buffer,
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

    /* Seed-driven phase offsets so different boots give different blob
     * geometry. Phases stay constant across the animation — the field is
     * static, only the threshold moves. */
    uint32_t h = ctx->seed * 2654435761u;
    float p1 = (float)((h >>  0) & 0xFFu) * (6.28318530717958647692f / 256.0f);
    float p2 = (float)((h >>  8) & 0xFFu) * (6.28318530717958647692f / 256.0f);
    float p3 = (float)((h >> 16) & 0xFFu) * (6.28318530717958647692f / 256.0f);

    float threshold = intro_anim_smoothstep(t);   /* 0..1 */

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            /* Plasma value at (sx, sy). Three orthogonal terms keyed at
             * different scales; sum is in [-3, 3], normalized to [0, 1]. */
            float fx = (float)sx;
            float fy = (float)sy;
            float v = sinf(fx * 0.32f + p1)
                    + sinf(fy * 0.28f + p2)
                    + sinf((fx + fy) * 0.22f + p3);
            float u = (v + 3.0f) * (1.0f / 6.0f);

            if (u > threshold) continue;

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
