// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* venetian: horizontal strips of source-rows slide in from alternating sides
 * (left, right, left, right, ...) and lock into place. Each strip's offset
 * fades to 0 by t=1 via smoothstep. Strips travel from off the screen
 * edge in their starting direction so they don't appear pre-positioned at
 * t=0+. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_V_STRIP_SRC 6     /* strip thickness in source-pixel rows */

void ia_venetian_render(uint8_t *buffer,
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

    /* Slide distance: enough that the strip is offscreen at t=0+. */
    const int slide_dist = ctx->width;
    float s = intro_anim_smoothstep(t);
    int travel = (int)((float)slide_dist * (1.0f - s) + 0.5f);  /* remaining */

    for (int oy = 0; oy < r.rotated_h; oy++) {
        int strip_idx = oy / IA_V_STRIP_SRC;
        int dir = (strip_idx & 1) ? +1 : -1;   /* even strips slide from left (-) */
        int row_off = dir * travel;

        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            int dx0 = ctx->logo_x + ox * scale + row_off;
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
