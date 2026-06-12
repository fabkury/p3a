// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file intro_anim.h
 * @brief Shared intro-animation interface (pure C, zero ESP-IDF includes).
 *
 * Each intro animation is a pure function of (buffer, ctx, t). The manager
 * (p3a_boot_logo.c on device, viewer_win32.c on host) owns the clock, the
 * blank-delay/hold bookends, and the choice of animation; the modules below
 * paint exactly one frame given a normalized time t in [0, 1].
 *
 * Contract enforced by host/intro-anim-lab/checks.c:
 *  - t = 0  : frame is pixel-identical to a flat fill of the bg color.
 *  - t = 1  : frame is pixel-identical to bg + opaque centered logo blit
 *             at scale `logo_scale` and rotation `rotation`.
 *  - same (ctx, t) deterministically produces the same bytes (no statics,
 *    no wall clock, no global RNG; all randomness from ctx->seed).
 *  - no writes outside (buffer, buffer + height*stride).
 */

#ifndef INTRO_ANIM_H
#define INTRO_ANIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      width;          /* 720 on device */
    int      height;         /* 720 on device */
    size_t   stride;         /* bytes per row, BGR888 */
    uint8_t  bg_r;
    uint8_t  bg_g;
    uint8_t  bg_b;
    uint16_t rotation;       /* 0 / 90 / 180 / 270 (clockwise) */
    int      logo_x;         /* top-left of the centered scaled+rotated logo */
    int      logo_y;
    int      logo_scale;     /* 3 on device */
    uint32_t seed;           /* per-boot seed; sole randomness source */
} intro_anim_ctx_t;

typedef void (*intro_anim_render_fn)(uint8_t *buffer,
                                     const intro_anim_ctx_t *ctx,
                                     float t);

typedef struct {
    const char           *name;             /* kebab-case, stable identifier */
    int                   frame_budget_ms;  /* target ms per frame; 33 = 30 FPS */
    intro_anim_render_fn  render;
} intro_anim_t;

extern const intro_anim_t intro_anim_registry[];
extern const int          intro_anim_count;

/* Helpers shared by animation modules. */
void intro_anim_fill_bg(uint8_t *buffer, const intro_anim_ctx_t *ctx);

/* Rotation coefficients used to walk dest coords back to source-logo coords.
 *   src_x = base_sx + cx_ox * ox + cx_oy * oy
 *   src_y = base_sy + cy_ox * ox + cy_oy * oy
 * where (ox, oy) is the unscaled-but-rotated dest offset from (logo_x, logo_y).
 * Matches p3a_logo_blit_pixelwise_bgr888's internal mapping verbatim. */
typedef struct {
    int base_sx, cx_ox, cx_oy;
    int base_sy, cy_ox, cy_oy;
    int rotated_w;   /* logo width after rotation, in source-pixel units */
    int rotated_h;
} intro_anim_rot_t;

void intro_anim_rot_init(intro_anim_rot_t *out, uint16_t rotation);

/* Stateless 32-bit hash, for per-pixel/per-tile randomness derived from the
 * per-boot seed. splitmix32-flavoured; cheap enough to call per pixel. */
uint32_t intro_anim_hash3(uint32_t seed, uint32_t a, uint32_t b);

/* Smoothstep, shared. */
float intro_anim_smoothstep(float t);

#ifdef __cplusplus
}
#endif

#endif /* INTRO_ANIM_H */
