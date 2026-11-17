// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* glitch-settle: horizontal slabs of source-rows are shifted left/right with
 * per-channel R/G/B offsets (chromatic aberration) and re-shuffled in
 * coarse "frame chunks" so the picture jitters and re-settles. All offsets
 * decay to 0 by t=1.
 *
 * Quantizing t into 12 chunks for the shake (rather than reshuffling every
 * frame) keeps the glitch readable instead of buzzing. The decay envelope
 * (1-t)^2 is continuous so the amplitude smoothly drops to zero.
 *
 * Determinism: offsets come from intro_anim_hash3(seed, slab, chunk_index),
 * so same (ctx, t) → same bytes. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_GS_SLAB_SRC   4    /* slab thickness in source rows */
#define IA_GS_CHUNKS     12   /* discrete shake states across [0, 1) */
#define IA_GS_AMP_DEST   18   /* peak slab offset (dest px) at t=0+ */
#define IA_GS_CHROMA_AMP 6    /* peak per-channel split (dest px) at t=0+ */

static int signed_off(uint32_t h, int amp_max)
{
    /* Map low bits to a signed offset in [-amp_max, +amp_max]. */
    int v = (int)(h & 0xFFFFu);
    int range = 2 * amp_max + 1;
    int o = (v % range) - amp_max;
    return o;
}

static const uint8_t *sample_logo_at(const intro_anim_rot_t *r, int ox, int oy)
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

void ia_glitch_settle_render(uint8_t *buffer,
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

    /* Decay envelope: (1-t)^2, monotonically -> 0 at t=1. */
    float v = 1.0f - t;
    float decay = v * v;
    int amp_slab   = (int)((float)IA_GS_AMP_DEST   * decay + 0.5f);
    int amp_chroma = (int)((float)IA_GS_CHROMA_AMP * decay + 0.5f);

    int chunk = (int)(t * (float)IA_GS_CHUNKS);
    if (chunk >= IA_GS_CHUNKS) chunk = IA_GS_CHUNKS - 1;

    int n_slabs = (r.rotated_h + IA_GS_SLAB_SRC - 1) / IA_GS_SLAB_SRC;

    /* Precompute per-slab offsets so the inner loop is just lookups. */
    int slab_off[32];   /* upper bound: 54/4 = 14 + slack */
    int r_off[32], g_off[32], b_off[32];
    if (n_slabs > 32) n_slabs = 32;
    for (int i = 0; i < n_slabs; i++) {
        uint32_t hs = intro_anim_hash3(ctx->seed, (uint32_t)i, (uint32_t)chunk);
        slab_off[i] = signed_off(hs, amp_slab);
        r_off[i] = signed_off(intro_anim_hash3(ctx->seed, (uint32_t)i, (uint32_t)(chunk + 1000)), amp_chroma);
        g_off[i] = signed_off(intro_anim_hash3(ctx->seed, (uint32_t)i, (uint32_t)(chunk + 2000)), amp_chroma);
        b_off[i] = signed_off(intro_anim_hash3(ctx->seed, (uint32_t)i, (uint32_t)(chunk + 3000)), amp_chroma);
    }

    int rw = r.rotated_w * scale;
    int rh = r.rotated_h * scale;
    int x0 = ctx->logo_x, y0 = ctx->logo_y, x1 = x0 + rw, y1 = y0 + rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ctx->width)  x1 = ctx->width;
    if (y1 > ctx->height) y1 = ctx->height;

    for (int dy = y0; dy < y1; dy++) {
        int oy = (dy - ctx->logo_y) / scale;
        if (oy < 0 || oy >= r.rotated_h) continue;
        int slab = oy / IA_GS_SLAB_SRC;
        if (slab < 0) slab = 0;
        if (slab >= n_slabs) slab = n_slabs - 1;

        int s_off = slab_off[slab];
        int rch = r_off[slab], gch = g_off[slab], bch = b_off[slab];

        uint8_t *row = buffer + (size_t)dy * ctx->stride;
        for (int dx = x0; dx < x1; dx++) {
            int ox_base_dest = dx - ctx->logo_x - s_off;
            int ox_base = ox_base_dest / scale;
            /* Per-channel sample positions in source-pixel units. */
            int ox_b = ox_base + bch / scale;
            int ox_g = ox_base + gch / scale;
            int ox_r = ox_base + rch / scale;

            const uint8_t *sb = sample_logo_at(&r, ox_b, oy);
            const uint8_t *sg = sample_logo_at(&r, ox_g, oy);
            const uint8_t *sr = sample_logo_at(&r, ox_r, oy);

            uint8_t *dst = row + dx * 3;
            /* Each channel's value: source channel if present, else bg.
             * Index 0=B, 1=G, 2=R. */
            dst[0] = sb ? sb[0] : ctx->bg_b;
            dst[1] = sg ? sg[1] : ctx->bg_g;
            dst[2] = sr ? sr[2] : ctx->bg_r;
        }
    }
}
