// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* voronoi-shatter: partition the rotated logo into ~24 Voronoi cells (L2
 * nearest-site assignment) and "shatter" them — each cell flies in from
 * its outward radial direction. By t=1 every cell has settled to its
 * home position so the canonical end state holds.
 *
 * Algorithm distinct: irregular polygonal tessellation with per-cell
 * rigid translation. Other animations either use regular grids
 * (checker-tiles, mosaic, venetian) or per-pixel motion (assemble,
 * pixel-rain, starburst). Cell-id lookup is computed once per frame
 * into static scratch (small enough to brute-force the nearest-site
 * loop: ~24 sites × ~2484 pixels = ~60k iters). */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_VS_SITES 24
#define IA_VS_MAX (46 * 54)

static int isqrt_dist2(int dx, int dy) { return dx * dx + dy * dy; }

void ia_voronoi_shatter_render(uint8_t *buffer,
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

    /* Site positions in rotated source coords, seeded. */
    int sites_x[IA_VS_SITES], sites_y[IA_VS_SITES];
    float fly_dx[IA_VS_SITES], fly_dy[IA_VS_SITES];
    float cx = (float)(r.rotated_w - 1) * 0.5f;
    float cy = (float)(r.rotated_h - 1) * 0.5f;
    for (int i = 0; i < IA_VS_SITES; i++) {
        uint32_t hx = intro_anim_hash3(ctx->seed, (uint32_t)i, 0xCA11u);
        uint32_t hy = intro_anim_hash3(ctx->seed, (uint32_t)i, 0xC0DEu);
        sites_x[i] = (int)(hx % (uint32_t)r.rotated_w);
        sites_y[i] = (int)(hy % (uint32_t)r.rotated_h);
        /* Outward radial direction in dest-px units (will multiply by
         * remaining-distance factor each frame). */
        float rdx = (float)sites_x[i] - cx;
        float rdy = (float)sites_y[i] - cy;
        float rr = sqrtf(rdx * rdx + rdy * rdy);
        if (rr < 0.001f) { rdx = 1.0f; rdy = 0.0f; rr = 1.0f; }
        /* Launch distance: a fraction of screen size, varied per cell. */
        float mag_var = 0.7f + (float)((hx >> 16) & 0xFFu) * (0.6f / 255.0f);
        float launch_dest = (float)(ctx->width > ctx->height ? ctx->width : ctx->height)
                          * 0.55f * mag_var;
        fly_dx[i] = (rdx / rr) * launch_dest;
        fly_dy[i] = (rdy / rr) * launch_dest;
    }

    /* Cubic ease-out so cells travel fast early and settle slowly. */
    float u = t;
    float v = 1.0f - u;
    float remain = v * v * v;   /* 1 → 0 */

    /* Cell-id map — shared freeable scratch, overwritten every call; freed
     * after the boot animation ends so it isn't held for the whole uptime. */
    uint8_t *cell_id = (uint8_t *)intro_anim_scratch(IA_VS_MAX);
    if (!cell_id) return;   /* alloc failed: leave the bg-filled frame */
    int W = r.rotated_w;
    int H = r.rotated_h;
    int N = W * H;
    /* Brute-force nearest-site assignment in rotated source coords. */
    for (int oy = 0; oy < H; oy++) {
        for (int ox = 0; ox < W; ox++) {
            int best = 0;
            int best_d2 = isqrt_dist2(ox - sites_x[0], oy - sites_y[0]);
            for (int i = 1; i < IA_VS_SITES; i++) {
                int d2 = isqrt_dist2(ox - sites_x[i], oy - sites_y[i]);
                if (d2 < best_d2) { best_d2 = d2; best = i; }
            }
            cell_id[oy * W + ox] = (uint8_t)best;
        }
    }
    (void)N;

    /* Render each pixel translated by its cell's current fly offset. */
    for (int oy = 0; oy < H; oy++) {
        for (int ox = 0; ox < W; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            int cid = cell_id[oy * W + ox];
            int off_x = (int)(fly_dx[cid] * remain);
            int off_y = (int)(fly_dy[cid] * remain);

            int dx0 = ctx->logo_x + ox * scale + off_x;
            int dy0 = ctx->logo_y + oy * scale + off_y;
            for (int by = 0; by < scale; by++) {
                int ddy = dy0 + by;
                if (ddy < 0 || ddy >= ctx->height) continue;
                uint8_t *row = buffer + (size_t)ddy * ctx->stride;
                for (int bx = 0; bx < scale; bx++) {
                    int ddx = dx0 + bx;
                    if (ddx < 0 || ddx >= ctx->width) continue;
                    uint8_t *dst = row + ddx * 3;
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                }
            }
        }
    }
}
