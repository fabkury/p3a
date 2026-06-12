// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/* smoothstep-fade: alpha = smoothstep(t) across the full window.
 * Reproduces the legacy boot animation when the manager's intro window is
 * 2000 ms (Phase 2 parity reference). */

#include "intro_anim.h"
#include "p3a_logo.h"

void ia_smoothstep_fade_render(uint8_t *buffer,
                               const intro_anim_ctx_t *ctx,
                               float t)
{
    intro_anim_fill_bg(buffer, ctx);

    float s = intro_anim_smoothstep(t);
    int alpha_i = (int)(s * 255.0f + 0.5f);
    if (alpha_i < 0) alpha_i = 0;
    if (alpha_i > 255) alpha_i = 255;

    p3a_logo_blit_pixelwise_bgr888(
        buffer, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, ctx->logo_y,
        (uint8_t)alpha_i,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);
}
