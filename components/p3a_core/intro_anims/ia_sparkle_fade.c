// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* sparkle-fade: smoothstep alpha fade like smoothstep-fade, plus seed-placed
 * twinkling sparkle dots over the bbox area that all extinguish before t=1.
 *
 * Sparkles: a fixed bag of N candidate positions in source-pixel space,
 * each with a personal birth window [b0, b0+life]. They draw as a small
 * cross (3-pixel plus-shape, in dest space) while alive. By t=1 every
 * sparkle is dead → endpoint matches canonical fully-opaque blit. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_SP_COUNT  40
#define IA_SP_LIFE   0.18f    /* sparkle visible window */

void ia_sparkle_fade_render(uint8_t *buffer,
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

    /* Step 1: smoothstep alpha fade. */
    intro_anim_fill_bg(buffer, ctx);
    float s = intro_anim_smoothstep(t);
    int alpha_i = (int)(s * 255.0f + 0.5f);
    if (alpha_i < 0) alpha_i = 0;
    if (alpha_i > 255) alpha_i = 255;
    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, ctx->logo_y, (uint8_t)alpha_i,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);

    /* Step 2: sparkles on top. Plus-shape so they look like twinkles
     * even at small sizes. */
    intro_anim_rot_t r;
    intro_anim_rot_init(&r, ctx->rotation);
    const int scale = ctx->logo_scale;
    int rw = r.rotated_w * scale;
    int rh = r.rotated_h * scale;

    /* Sparkle color: contrast against bg. */
    int lum = (int)ctx->bg_r * 299 + (int)ctx->bg_g * 587 + (int)ctx->bg_b * 114;
    uint8_t spk_v = (lum < 127500) ? 255 : 8;

    for (int i = 0; i < IA_SP_COUNT; i++) {
        uint32_t hp = intro_anim_hash3(ctx->seed, (uint32_t)i, 0xA15Eu);
        uint32_t ht = intro_anim_hash3(ctx->seed, (uint32_t)i, 0x71D5u);

        /* Position within bbox, dest-space. */
        int px = ctx->logo_x + (int)((hp & 0xFFFFu) % (uint32_t)rw);
        int py = ctx->logo_y + (int)(((hp >> 16) & 0xFFFFu) % (uint32_t)rh);

        /* Birth window — uniformly distributed but constrained so death
         * <= 1 - epsilon, ensuring no sparkle survives to t=1. */
        float b0 = (float)(ht & 0xFFFFu) / 65535.0f * (1.0f - IA_SP_LIFE - 0.02f);
        if (t < b0 || t >= b0 + IA_SP_LIFE) continue;

        /* Fade by triangular envelope across life. */
        float u = (t - b0) / IA_SP_LIFE;
        float env = (u < 0.5f) ? (u * 2.0f) : ((1.0f - u) * 2.0f);
        if (env <= 0.05f) continue;

        /* Plus-shape: center + 4 neighbors at 1 dest-px. */
        static const int dxs[5] = { 0, -1, +1,  0,  0 };
        static const int dys[5] = { 0,  0,  0, -1, +1 };
        for (int k = 0; k < 5; k++) {
            int dx = px + dxs[k];
            int dy = py + dys[k];
            if (dx < 0 || dx >= ctx->width || dy < 0 || dy >= ctx->height) continue;
            uint8_t *dst = buffer + (size_t)dy * ctx->stride + dx * 3;
            /* Blend toward sparkle color by env. */
            int a = (int)(env * 255.0f + 0.5f);
            int ia = 255 - a;
            dst[0] = (uint8_t)((spk_v * a + dst[0] * ia + 127) / 255);
            dst[1] = (uint8_t)((spk_v * a + dst[1] * ia + 127) / 255);
            dst[2] = (uint8_t)((spk_v * a + dst[2] * ia + 127) / 255);
        }
    }
}
