// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* channel-merge: three ghost copies of the logo — one per color
 * channel — slide in from three directions 120 degrees apart and
 * converge on the home position. Each ghost writes only its own
 * channel byte, so while they are apart you see three tinted
 * silhouettes, and wherever they overlap the channels recombine
 * toward the true colors. Locks are staggered (R, then G, then B) so
 * the final fringe resolves with three little clicks.
 *
 * Algorithm distinct: additive channel composition. glitch-settle
 * jitters R/G/B slabs around the home position; this is a deliberate
 * long-range convergence of three complete channel planes. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_CM_DIST 0.85f   /* slide distance, x screen width */

void ia_channel_merge_render(uint8_t *buffer,
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

    /* Per-channel approach: direction (unit), lock time, byte index in
     * the BGR888 pixel. R comes from the left, G from the upper right,
     * B from the lower right; R lands first, B last. */
    static const struct {
        float dx, dy;
        float lock;
        int   byte_idx;   /* 0=B, 1=G, 2=R */
    } chan[3] = {
        { -1.0f,  0.0f,       0.78f, 2 },   /* R */
        {  0.5f, -0.8660254f, 0.88f, 1 },   /* G */
        {  0.5f,  0.8660254f, 0.98f, 0 },   /* B */
    };

    float dist = (float)ctx->width * IA_CM_DIST;

    for (int c = 0; c < 3; c++) {
        float u = t / chan[c].lock;
        if (u > 1.0f) u = 1.0f;
        float inv = 1.0f - u;
        float remain = inv * inv * inv;   /* cubic ease-out remainder */

        int off_x = (int)(chan[c].dx * dist * remain + (chan[c].dx < 0 ? -0.5f : 0.5f));
        int off_y = (int)(chan[c].dy * dist * remain + (chan[c].dy < 0 ? -0.5f : 0.5f));

        for (int oy = 0; oy < r.rotated_h; oy++) {
            for (int ox = 0; ox < r.rotated_w; ox++) {
                int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
                int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
                const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
                if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                    src[1] == P3A_LOGO_CHROMA_KEY_G &&
                    src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

                int dx0 = ctx->logo_x + ox * scale + off_x;
                int dy0 = ctx->logo_y + oy * scale + off_y;

                for (int by = 0; by < scale; by++) {
                    int dy = dy0 + by;
                    if (dy < 0 || dy >= ctx->height) continue;
                    uint8_t *row = buffer + (size_t)dy * ctx->stride;
                    for (int bx = 0; bx < scale; bx++) {
                        int dx = dx0 + bx;
                        if (dx < 0 || dx >= ctx->width) continue;
                        row[dx * 3 + chan[c].byte_idx] = src[chan[c].byte_idx];
                    }
                }
            }
        }
    }
}
