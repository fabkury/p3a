// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* shutter-bands: vertical column-bands open from the bbox center outward,
 * like a camera shutter or accordion. Each band has a fixed width B in
 * source columns; the *number of open bands* grows from 1 (innermost) to
 * cover-all by t=1. Within an open band, content is fully shown.
 *
 * Distinct from `venetian` (which slides full strips around), `iris-wipe`
 * (radial L2), and `diamond-wipe` (L1): here the geometry is a one-axis
 * symmetric wipe that opens *outward in column order*. Bands open at
 * smoothstep-paced rate. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_SB_BAND_SRC 4

void ia_shutter_bands_render(uint8_t *buffer,
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

    int W = r.rotated_w;
    int n_bands = (W + IA_SB_BAND_SRC - 1) / IA_SB_BAND_SRC;
    int center_band = n_bands / 2;

    /* Half-distance from center in band units to fully cover. */
    int reach = center_band > (n_bands - center_band) ? center_band : (n_bands - center_band);
    if (reach <= 0) reach = 1;

    float s = intro_anim_smoothstep(t);
    int open_reach = (int)((float)reach * s + 0.5f);

    for (int band = 0; band < n_bands; band++) {
        int dist = band - center_band;
        if (dist < 0) dist = -dist;
        if (dist > open_reach) continue;

        int ox0 = band * IA_SB_BAND_SRC;
        int ox1 = ox0 + IA_SB_BAND_SRC;
        if (ox1 > W) ox1 = W;

        for (int oy = 0; oy < r.rotated_h; oy++) {
            for (int ox = ox0; ox < ox1; ox++) {
                int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
                int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
                const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
                if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                    src[1] == P3A_LOGO_CHROMA_KEY_G &&
                    src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

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
}
