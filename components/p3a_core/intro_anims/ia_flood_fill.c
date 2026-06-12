// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* flood-fill: reveal opaque source pixels in 4-connected breadth-first
 * order from a deterministic seed pixel. The reveal "spreads" through the
 * logo like ink soaking outward — distinct from any radial/threshold
 * mechanism because it follows the actual logo topology.
 *
 * Algorithm per frame:
 *   1. Build the rotated source mask (opaque = 1, transparent = 0) as a
 *      flat (rotated_w * rotated_h) array.
 *   2. BFS from the first opaque pixel in row-major order. Record the
 *      visit order in `bfs_idx[i]` = packed (oy * rotated_w + ox).
 *   3. K = smoothstep(t) * N_visited; emit the first K visits.
 *
 * Memory: function-local static arrays bounded by p3a_logo_w * p3a_logo_h
 * = 2484 entries. Per-frame cost is O(N) BFS + K visit emit; the manager's
 * 40 ms budget at 720x720 has plenty of room for this.
 *
 * Determinism: BFS visit order from a fixed seed in a fixed mask is fully
 * deterministic. We don't even need ctx->seed here. */

#include "intro_anim.h"
#include "p3a_logo.h"

#define IA_FF_MAX (46 * 54)

void ia_flood_fill_render(uint8_t *buffer,
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
    const int W = r.rotated_w;
    const int H = r.rotated_h;
    const int N = W * H;

    /* Per-call scratch (function-local static; overwritten every call). */
    static uint8_t  mask[IA_FF_MAX];
    static int      bfs_q[IA_FF_MAX];     /* visit queue (FIFO) */
    static uint8_t  visited[IA_FF_MAX];

    /* Build mask + clear visited. */
    for (int oy = 0; oy < H; oy++) {
        for (int ox = 0; ox < W; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            int op = !(src[0] == P3A_LOGO_CHROMA_KEY_B &&
                       src[1] == P3A_LOGO_CHROMA_KEY_G &&
                       src[2] == P3A_LOGO_CHROMA_KEY_R);
            mask[oy * W + ox] = (uint8_t)op;
            visited[oy * W + ox] = 0;
        }
    }

    /* Find first opaque pixel in row-major order = BFS seed. */
    int seed = -1;
    for (int i = 0; i < N; i++) {
        if (mask[i]) { seed = i; break; }
    }
    if (seed < 0) return;

    /* BFS. Treats the connected component containing the seed only — if
     * the logo has disjoint regions, K stays scaled to that component's
     * size and the rest never reveal during the animation, which is fine:
     * the canonical t=1 path takes over for the final frame. */
    int qh = 0, qt = 0;
    bfs_q[qt++] = seed;
    visited[seed] = 1;
    while (qh < qt) {
        int cur = bfs_q[qh++];
        int cx = cur % W, cy = cur / W;
        static const int dxs[4] = { -1, +1, 0, 0 };
        static const int dys[4] = {  0,  0, -1, +1 };
        for (int k = 0; k < 4; k++) {
            int nx = cx + dxs[k], ny = cy + dys[k];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            int ni = ny * W + nx;
            if (visited[ni] || !mask[ni]) continue;
            visited[ni] = 1;
            bfs_q[qt++] = ni;
        }
    }

    /* Emit first K visits in BFS order. */
    int total = qt;
    float s = intro_anim_smoothstep(t);
    int K = (int)((float)total * s + 0.5f);
    if (K <= 0) return;
    if (K > total) K = total;

    for (int i = 0; i < K; i++) {
        int idx = bfs_q[i];
        int ox = idx % W, oy = idx / W;
        int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
        int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
        const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;

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
