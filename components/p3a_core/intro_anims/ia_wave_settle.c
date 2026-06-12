// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* wave-settle: each source-row of the logo is displaced horizontally by a
 * sine wave whose amplitude damps to zero by t=1, while the logo's overall
 * alpha fades in via smoothstep. The wave's phase advances over time so the
 * displacement reads as motion settling, not a frozen S-curve.
 *
 * Pixel-art preserved: row displacement is an integer source-pixel count
 * (scaled to dest), so each 3x3 block shifts as a unit; rows still align
 * vertically. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_WS_AMP_SRC_PX  6     /* peak displacement at t=0+, in source px */
#define IA_WS_PERIOD_SRC  18.0f /* wave period in source rows */
#define IA_WS_CYCLES      2.0f  /* full wave cycles traversed by t=1 */

void ia_wave_settle_render(uint8_t *buffer,
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

    /* Linear-damped amplitude; phase advances forward in t so the wave
     * appears to drift while shrinking. */
    float amp_src = (float)IA_WS_AMP_SRC_PX * (1.0f - t);
    float phase   = IA_WS_CYCLES * 6.28318530717958647692f * t;

    /* Alpha blend strength fades in via smoothstep(t). */
    float s = intro_anim_smoothstep(t);
    int alpha_i = (int)(s * 255.0f + 0.5f);
    if (alpha_i < 0) alpha_i = 0;
    if (alpha_i > 255) alpha_i = 255;
    int a = alpha_i, ia = 255 - alpha_i;

    /* Precompute per-source-row dest offsets (rotated_h <= 54). */
    int row_off_dest[64];
    for (int oy = 0; oy < r.rotated_h; oy++) {
        float w = sinf(((float)oy / IA_WS_PERIOD_SRC) * 6.28318530717958647692f + phase);
        int off_src = (int)(amp_src * w + (w >= 0.0f ? 0.5f : -0.5f));
        row_off_dest[oy] = off_src * scale;
    }

    for (int oy = 0; oy < r.rotated_h; oy++) {
        int dy0 = ctx->logo_y + oy * scale;
        int row_off = row_off_dest[oy];
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            int dx0 = ctx->logo_x + ox * scale + row_off;

            for (int by = 0; by < scale; by++) {
                int dy = dy0 + by;
                if (dy < 0 || dy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)dy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int dx = dx0 + bx;
                    if (dx < 0 || dx >= ctx->width) continue;
                    uint8_t *dst = row + dx * 3;
                    /* Blend src with bg using alpha (255-based, rounded). */
                    dst[0] = (uint8_t)((src[0] * a + ctx->bg_b * ia + 127) / 255);
                    dst[1] = (uint8_t)((src[1] * a + ctx->bg_g * ia + 127) / 255);
                    dst[2] = (uint8_t)((src[2] * a + ctx->bg_r * ia + 127) / 255);
                }
            }
        }
    }
}
