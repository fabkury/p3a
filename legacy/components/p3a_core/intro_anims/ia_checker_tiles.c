// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* checker-tiles: split the rotated logo into 6x6 source-pixel tiles and pop
 * them in in shuffled order. Hash-priority reveal at tile granularity (same
 * trick as pixel-dissolve, but per tile) — gives a chunky, checkerboard-y
 * feel rather than per-pixel speckle.
 *
 * Tile choice (6 src px = 18 dest px) keeps tiles visually distinct without
 * being so coarse that the count drops below ~50. Per-tile ease is
 * smoothstep(t) so reveal accelerates and decelerates. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <stdlib.h>

#define IA_CT_TILE_SRC 6
/* Upper bound on tile count: ceil(54/6)*ceil(46/6) = 9*8 = 72. Round up for
 * safety. */
#define IA_CT_MAX_TILES 96

static int cmp_u32(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

void ia_checker_tiles_render(uint8_t *buffer,
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

    int tiles_x = (r.rotated_w + IA_CT_TILE_SRC - 1) / IA_CT_TILE_SRC;
    int tiles_y = (r.rotated_h + IA_CT_TILE_SRC - 1) / IA_CT_TILE_SRC;
    int N = tiles_x * tiles_y;

    /* Hashes — shared freeable scratch, fully overwritten each call; freed
     * after the boot animation ends so it isn't held for the whole uptime. */
    uint32_t *hashes = (uint32_t *)intro_anim_scratch(sizeof(uint32_t) * IA_CT_MAX_TILES);
    if (!hashes) return;   /* alloc failed: leave the bg-filled frame */
    if (N > IA_CT_MAX_TILES) N = IA_CT_MAX_TILES;
    for (int i = 0; i < N; i++) {
        int tx = i % tiles_x;
        int ty = i / tiles_x;
        hashes[i] = intro_anim_hash3(ctx->seed, (uint32_t)tx, (uint32_t)ty);
    }

    float s = intro_anim_smoothstep(t);
    int K = (int)((float)N * s + 0.5f);
    if (K <= 0) return;
    if (K > N) K = N;

    qsort(hashes, (size_t)N, sizeof(uint32_t), cmp_u32);
    uint32_t threshold = hashes[K - 1];

    /* Walk tiles in row-major order; for each enabled tile draw all its
     * opaque source pixels. */
    for (int ty = 0; ty < tiles_y; ty++) {
        for (int tx = 0; tx < tiles_x; tx++) {
            uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)tx, (uint32_t)ty);
            if (h > threshold) continue;

            int ox0 = tx * IA_CT_TILE_SRC;
            int oy0 = ty * IA_CT_TILE_SRC;
            int ox1 = ox0 + IA_CT_TILE_SRC;
            int oy1 = oy0 + IA_CT_TILE_SRC;
            if (ox1 > r.rotated_w) ox1 = r.rotated_w;
            if (oy1 > r.rotated_h) oy1 = r.rotated_h;

            for (int oy = oy0; oy < oy1; oy++) {
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
                            dst[0] = src[0];
                            dst[1] = src[1];
                            dst[2] = src[2];
                        }
                    }
                }
            }
        }
    }
}
