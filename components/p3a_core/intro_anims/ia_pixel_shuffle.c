// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* pixel-shuffle: every dest position within the bbox samples the logo
 * with a per-pixel random Cartesian offset (in source-pixel units) that
 * decays to 0 by t=1. Distinct from `assemble` because the offset is
 * **2D random** not radial-from-center, and from `wave-settle` because
 * it's per-pixel not per-row. The image looks "scrambled" early and
 * unscrambles in place.
 *
 * To preserve the pixel-art block identity, all 3x3 dest sub-pixels
 * within one source pixel share the same offset. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_PSH_AMP_SRC 8   /* peak per-pixel offset, source-px units */

static int signed_off(uint32_t h, int amp_max)
{
    int v = (int)(h & 0xFFFFu);
    int range = 2 * amp_max + 1;
    return (v % range) - amp_max;
}

static const uint8_t *sample_or_null(const intro_anim_rot_t *r, int ox, int oy)
{
    if (ox < 0 || oy < 0 || ox >= r->rotated_w || oy >= r->rotated_h) return NULL;
    int sx = r->base_sx + r->cx_ox * ox + r->cx_oy * oy;
    int sy = r->base_sy + r->cy_ox * ox + r->cy_oy * oy;
    const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
    if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
        src[1] == P3A_LOGO_CHROMA_KEY_G &&
        src[2] == P3A_LOGO_CHROMA_KEY_R) return NULL;
    return src;
}

void ia_pixel_shuffle_render(uint8_t *buffer,
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

    /* Decay envelope: linear (1 - t) is plenty here since the visual
     * mechanism is randomness, not motion. */
    float decay = 1.0f - t;
    int amp = (int)((float)IA_PSH_AMP_SRC * decay + 0.5f);

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            uint32_t hx = intro_anim_hash3(ctx->seed, (uint32_t)(ox * 7919 + oy), 1u);
            uint32_t hy = intro_anim_hash3(ctx->seed, (uint32_t)(ox * 7919 + oy), 2u);
            int doff_x = signed_off(hx, amp);
            int doff_y = signed_off(hy, amp);

            const uint8_t *src = sample_or_null(&r, ox + doff_x, oy + doff_y);
            if (!src) continue;

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
