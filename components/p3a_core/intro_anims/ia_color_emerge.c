// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* color-emerge: the logo silhouette appears in a flat contrast color
 * first, then per-pixel each channel value linearly interpolates from
 * the silhouette color toward the source-pixel's true color.
 *
 * Algorithm distinct from any other animation in the roster: it animates
 * in *color space* with full opacity from t=0+, no positional motion, no
 * mask reveal. Three phases on t:
 *   t in [0.00, 0.25): silhouette alpha-fades in via smoothstep on
 *                      remapped t' = t/0.25.
 *   t in [0.25, 1.00]: per-channel lerp from silhouette to source by
 *                      u = (t - 0.25) / 0.75.
 *
 * Silhouette color: contrast against bg luminance (white on dark bgs,
 * near-black on light bgs). */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_color_emerge_render(uint8_t *buffer,
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

    int lum = (int)ctx->bg_r * 299 + (int)ctx->bg_g * 587 + (int)ctx->bg_b * 114;
    uint8_t sil_v = (lum < 127500) ? 240 : 24;

    int alpha;     /* silhouette opacity vs bg, 0..255 */
    int color_u;   /* silhouette->source lerp, 0..255 (at 255 = full source) */
    if (t < 0.25f) {
        float ts = t / 0.25f;
        float ss = ts * ts * (3.0f - 2.0f * ts);   /* smoothstep */
        alpha = (int)(ss * 255.0f + 0.5f);
        color_u = 0;
    } else {
        alpha = 255;
        float u = (t - 0.25f) / 0.75f;
        color_u = (int)(u * 255.0f + 0.5f);
    }
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    if (color_u < 0) color_u = 0;
    if (color_u > 255) color_u = 255;
    int ialpha = 255 - alpha;
    int icu = 255 - color_u;

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            /* Per-channel: silhouette->source lerp, then bg-blend by alpha. */
            int b_mid = (sil_v * icu + src[0] * color_u + 127) / 255;
            int g_mid = (sil_v * icu + src[1] * color_u + 127) / 255;
            int r_mid = (sil_v * icu + src[2] * color_u + 127) / 255;

            int b_out = (b_mid * alpha + ctx->bg_b * ialpha + 127) / 255;
            int g_out = (g_mid * alpha + ctx->bg_g * ialpha + 127) / 255;
            int r_out = (r_mid * alpha + ctx->bg_r * ialpha + 127) / 255;

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
                    dst[0] = (uint8_t)b_out;
                    dst[1] = (uint8_t)g_out;
                    dst[2] = (uint8_t)r_out;
                }
            }
        }
    }
}
