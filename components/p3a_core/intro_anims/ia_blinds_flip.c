// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* blinds-flip: horizontal strips of the logo flip open like the slats
 * of a venetian blind rotating from edge-on to face-on. Each strip
 * grows vertically from nothing around its center row (foreshortening:
 * content squashed by the flip factor, nearest-sampled so the pixel
 * art stays blocky) and brightens from a dimmed edge-on shade to full
 * color. Strips open top-to-bottom on a staggered schedule.
 *
 * Algorithm distinct from venetian (strips slide horizontally, no
 * scaling) and scanline-reveal (binary visibility sweep): this is a
 * per-strip vertical foreshortening with shading — the only animation
 * in the roster that "rotates" geometry in pseudo-3D. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_BF_STRIP_SRC 6      /* strip thickness in rotated source rows */
#define IA_BF_DUR       0.45f  /* each strip's open window, in t units */
#define IA_BF_SHADE_MIN 0.35f  /* brightness at edge-on (f -> 0) */

void ia_blinds_flip_render(uint8_t *buffer,
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

    int nstrips = (r.rotated_h + IA_BF_STRIP_SRC - 1) / IA_BF_STRIP_SRC;
    float spread = 1.0f - IA_BF_DUR;   /* last strip starts here */

    for (int k = 0; k < nstrips; k++) {
        int strip_y0 = k * IA_BF_STRIP_SRC;
        int strip_len = r.rotated_h - strip_y0;
        if (strip_len > IA_BF_STRIP_SRC) strip_len = IA_BF_STRIP_SRC;

        float start = (nstrips > 1) ? spread * (float)k / (float)(nstrips - 1) : 0.0f;
        float u = (t - start) / IA_BF_DUR;
        if (u <= 0.0f) continue;            /* still edge-on: invisible */
        if (u > 1.0f) u = 1.0f;
        float f = intro_anim_smoothstep(u); /* flip-open factor 0..1 */
        if (f <= 0.0f) continue;

        float shade = IA_BF_SHADE_MIN + (1.0f - IA_BF_SHADE_MIN) * f;
        float c = (float)strip_y0 + (float)(strip_len - 1) * 0.5f;

        for (int oy = strip_y0; oy < strip_y0 + strip_len; oy++) {
            /* Foreshortened sampling: display row oy shows the strip's
             * content squashed by f around the strip center. */
            float d = (float)oy - c;
            float syf = c + d / f;
            int s_oy = (int)floorf(syf + 0.5f);
            if (s_oy < strip_y0 || s_oy >= strip_y0 + strip_len) continue;

            for (int ox = 0; ox < r.rotated_w; ox++) {
                int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * s_oy;
                int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * s_oy;
                const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
                if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                    src[1] == P3A_LOGO_CHROMA_KEY_G &&
                    src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

                uint8_t out_b = (uint8_t)((float)src[0] * shade + 0.5f);
                uint8_t out_g = (uint8_t)((float)src[1] * shade + 0.5f);
                uint8_t out_r = (uint8_t)((float)src[2] * shade + 0.5f);

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
                        dst[0] = out_b;
                        dst[1] = out_g;
                        dst[2] = out_r;
                    }
                }
            }
        }
    }
}
