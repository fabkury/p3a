// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* neon-trace: outline pixels (opaque source pixels with at least one
 * transparent or out-of-bounds 4-neighbor) light up clockwise around the
 * bbox center over the first half of t. Over the second half, interior
 * pixels fill in by hash-priority.
 *
 * Outline pixels are bright (a contrast color picked against bg) so the
 * trace reads as a glowing line; once filled, interior pixels show their
 * true source color and outline reverts to source color too.
 *
 * Walk uses cumulative angle: the swept angle range expands from 0 to 2*PI
 * during the trace half, so outline pixels stay lit once they enter the
 * range. */

#include "intro_anim.h"
#include "p3a_logo.h"
#include <math.h>

#define IA_NT_TRACE_END  0.55f  /* outline finishes by t = 0.55 */

static int is_opaque_at(const intro_anim_rot_t *r, int ox, int oy)
{
    if (ox < 0 || oy < 0 || ox >= r->rotated_w || oy >= r->rotated_h) return 0;
    int sx = r->base_sx + r->cx_ox * ox + r->cx_oy * oy;
    int sy = r->base_sy + r->cy_ox * ox + r->cy_oy * oy;
    const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
    return !(src[0] == P3A_LOGO_CHROMA_KEY_B &&
             src[1] == P3A_LOGO_CHROMA_KEY_G &&
             src[2] == P3A_LOGO_CHROMA_KEY_R);
}

void ia_neon_trace_render(uint8_t *buffer,
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

    /* Contrast color for the trace. */
    int lum = (int)ctx->bg_r * 299 + (int)ctx->bg_g * 587 + (int)ctx->bg_b * 114;
    uint8_t neon_v = (lum < 127500) ? 240 : 24;

    /* Trace fraction: 0 at t=0, 1 at t=IA_NT_TRACE_END. */
    float trace_u = t / IA_NT_TRACE_END;
    if (trace_u > 1.0f) trace_u = 1.0f;

    /* Fill fraction: 0 until t=IA_NT_TRACE_END, 1 at t=1. */
    float fill_u = 0.0f;
    if (t > IA_NT_TRACE_END) fill_u = (t - IA_NT_TRACE_END) / (1.0f - IA_NT_TRACE_END);

    /* Swept angle range [0, max_angle). Pixel angle measured from -PI
     * (top-left in screen-natural way) sweeping clockwise.  */
    const float TWOPI = 6.28318530717958647692f;
    float max_angle = TWOPI * trace_u;

    /* Center for angle measurement: bbox center in source-pixel coords. */
    float cx = (float)(r.rotated_w - 1) * 0.5f;
    float cy = (float)(r.rotated_h - 1) * 0.5f;

    for (int oy = 0; oy < r.rotated_h; oy++) {
        for (int ox = 0; ox < r.rotated_w; ox++) {
            int sx = r.base_sx + r.cx_ox * ox + r.cx_oy * oy;
            int sy = r.base_sy + r.cy_ox * ox + r.cy_oy * oy;
            const uint8_t *src = p3a_logo_pixels + ((size_t)sy * p3a_logo_w + sx) * 3;
            if (src[0] == P3A_LOGO_CHROMA_KEY_B &&
                src[1] == P3A_LOGO_CHROMA_KEY_G &&
                src[2] == P3A_LOGO_CHROMA_KEY_R) continue;

            /* Outline = at least one transparent/oob 4-neighbor. */
            int is_outline = (!is_opaque_at(&r, ox - 1, oy) ||
                              !is_opaque_at(&r, ox + 1, oy) ||
                              !is_opaque_at(&r, ox, oy - 1) ||
                              !is_opaque_at(&r, ox, oy + 1));

            int show = 0;
            int as_neon = 0;

            if (is_outline) {
                /* Outline pixel: lit once its angle is within the swept
                 * range. Angle measured atan2; remap to [0, 2PI). */
                float dxa = (float)ox - cx;
                float dya = (float)oy - cy;
                float a = atan2f(dya, dxa);   /* (-PI, PI] */
                float ar = a + 3.14159265358979323846f;  /* [0, 2PI) */
                if (ar < max_angle) {
                    show = 1;
                    /* Once filling starts, interior may have caught up; stop
                     * showing outline as neon and let it use src color. */
                    as_neon = (fill_u <= 0.0f);
                }
            } else {
                /* Interior: revealed proportionally during fill phase via a
                 * hash-priority threshold (cheap, no sort needed at this
                 * granularity — reuse the seed-hash). */
                if (fill_u > 0.0f) {
                    uint32_t h = intro_anim_hash3(ctx->seed, (uint32_t)sx, (uint32_t)sy);
                    float th = (float)h / 4294967295.0f;
                    if (th < fill_u) show = 1;
                }
            }

            if (!show) continue;

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
                    if (as_neon) {
                        dst[0] = neon_v; dst[1] = neon_v; dst[2] = neon_v;
                    } else {
                        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                    }
                }
            }
        }
    }
}
