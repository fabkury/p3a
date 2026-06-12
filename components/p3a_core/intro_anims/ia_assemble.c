// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* assemble: each opaque source pixel flies in along a continuous angle
 * (256 directions from a sin/cos LUT) at a per-pixel-varied distance, and
 * lands at its home position by t=1. Per-source pixel motion → pixel-art
 * preserved. Distance decays with cubic ease-out: fast travel early,
 * slow settle at the end.
 *
 * The LUT is one-time initialized on first call; contents are
 * deterministic and identical across calls, so the (ctx, t) contract
 * still holds.
 */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_DIR_LUT_N 256

static float ia_dir_lut[IA_DIR_LUT_N * 2];   /* [cos0, sin0, cos1, sin1, ...] */
static int   ia_dir_lut_ready = 0;

static void ensure_dir_lut(void)
{
    if (ia_dir_lut_ready) return;
    for (int i = 0; i < IA_DIR_LUT_N; i++) {
        float a = (float)i * (6.28318530717958647692f / (float)IA_DIR_LUT_N);
        ia_dir_lut[i * 2 + 0] = cosf(a);
        ia_dir_lut[i * 2 + 1] = sinf(a);
    }
    ia_dir_lut_ready = 1;
}

void ia_assemble_render(uint8_t *buffer,
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

    ensure_dir_lut();
    intro_anim_fill_bg(buffer, ctx);

    intro_anim_rot_t r;
    intro_anim_rot_init(&r, ctx->rotation);
    const int scale = ctx->logo_scale;

    /* Cubic ease-out: u = 1 - (1-t)^3; particles cover most ground early
     * and settle slowly. decay = 1 - u is the remaining fraction of the
     * fly distance at this frame. */
    float u = 1.0f - t;
    u = 1.0f - u * u * u;
    float decay = 1.0f - u;

    int fly_dist = (ctx->width > ctx->height ? ctx->width : ctx->height);
    float base_mag = (float)fly_dist * decay;

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);

            /* Continuous angle: low byte indexes the LUT (1.4° resolution). */
            int idx = (int)(h & 0xFFu);
            float cos_a = ia_dir_lut[idx * 2 + 0];
            float sin_a = ia_dir_lut[idx * 2 + 1];

            /* Per-pixel magnitude variance in [0.6, 1.4]: next byte of hash.
             * Even pixels with similar angles arrive at different times. */
            uint32_t mag_byte = (h >> 8) & 0xFFu;
            float mag_var = 0.6f + (float)mag_byte * (0.8f / 255.0f);
            float mag = base_mag * mag_var;

            int off_x = (int)(cos_a * mag);
            int off_y = (int)(sin_a * mag);

            int dx0 = ctx->logo_x + ox * scale + off_x;
            int dy0 = ctx->logo_y + oy * scale + off_y;

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
