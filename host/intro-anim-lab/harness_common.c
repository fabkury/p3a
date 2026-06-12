// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "harness.h"
#include "p3a_logo.h"

extern const int p3a_logo_w;
extern const int p3a_logo_h;

void harness_compute_logo_position(intro_anim_ctx_t *ctx)
{
    int logo_w, logo_h;
    if (ctx->rotation == 90 || ctx->rotation == 270) {
        logo_w = p3a_logo_h * ctx->logo_scale;
        logo_h = p3a_logo_w * ctx->logo_scale;
    } else {
        logo_w = p3a_logo_w * ctx->logo_scale;
        logo_h = p3a_logo_h * ctx->logo_scale;
    }
    ctx->logo_x = (ctx->width  - logo_w) / 2;
    ctx->logo_y = (ctx->height - logo_h) / 2;
}

const harness_bg_preset_t harness_bg_presets[] = {
    { "black",       0x00, 0x00, 0x00 },
    { "white",       0xff, 0xff, 0xff },
    { "p3a-orange",  0xff, 0x80, 0x00 },
    { "deep-blue",   0x10, 0x30, 0x80 },
    { "chroma-gray", 0x80, 0x80, 0x80 },
};
const int harness_bg_preset_count =
    (int)(sizeof(harness_bg_presets) / sizeof(harness_bg_presets[0]));
