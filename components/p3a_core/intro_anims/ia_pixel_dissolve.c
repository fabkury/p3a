// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* pixel-dissolve: at any time t, exactly K (≈ smoothstep(t) * N_opaque) of
 * the opaque source-logo pixels are visible. The choice of WHICH K is
 * priority-by-hash: each opaque pixel gets a stable seeded hash; the K
 * pixels with the lowest hashes are shown.
 *
 * Why hash-ranking instead of a multiplicative permutation: a multiplicative
 * permutation walks the bitmap in a fixed stride, which the eye reads as a
 * traveling wavefront. Hashing scatters cleanly — pattern looks random.
 *
 * One hash per source pixel, so each 3×3 scaled block flips together —
 * the pixel-art chunkiness stays.
 */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <stdlib.h>

/* Upper bound on opaque-pixel count = p3a_logo_w * p3a_logo_h. */
#define IA_PD_MAX_OPAQUE (46 * 54)

static int cmp_u32(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

void ia_pixel_dissolve_render(uint8_t *buffer,
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

    /* Scratch — function-local static, fully overwritten each call (no
     * state persists across frames; determinism contract intact). */
    static uint32_t hashes[IA_PD_MAX_OPAQUE];
    int N = 0;

    /* Pass 1: collect a hash per opaque pixel. */
    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;
            hashes[N++] = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
        }
    }
    if (N <= 0) return;

    float s = intro_anim_smoothstep(t);
    int K = (int)((float)N * s + 0.5f);
    if (K <= 0) return;
    if (K > N) K = N;

    /* Sort, take K-th smallest as the visibility threshold. With 32-bit
     * hashes over N ≤ 2484, ties at the threshold happen with probability
     * ~7e-7 per call — accept the rare ±1 drift. */
    qsort(hashes, (size_t)N, sizeof(uint32_t), cmp_u32);
    uint32_t threshold = hashes[K - 1];

    /* Pass 2: walk same order, recompute hash, draw if <= threshold. */
    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
            if (h > threshold) continue;

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
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
        }
    }
}
