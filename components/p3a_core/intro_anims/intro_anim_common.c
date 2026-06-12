// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "intro_anim.h"
#include "p3a_logo.h"

void intro_anim_fill_bg(uint8_t *buffer, const intro_anim_ctx_t *ctx)
{
    for (int y = 0; y < ctx->height; y++) {
        uint8_t *row = buffer + (size_t)y * ctx->stride;
        for (int x = 0; x < ctx->width; x++) {
            row[x * 3 + 0] = ctx->bg_b;
            row[x * 3 + 1] = ctx->bg_g;
            row[x * 3 + 2] = ctx->bg_r;
        }
    }
}

void intro_anim_rot_init(intro_anim_rot_t *out, uint16_t rotation)
{
    if (rotation == 90) {
        out->base_sx = 0;              out->cx_ox =  0; out->cx_oy =  1;
        out->base_sy = p3a_logo_h - 1; out->cy_ox = -1; out->cy_oy =  0;
        out->rotated_w = p3a_logo_h;
        out->rotated_h = p3a_logo_w;
    } else if (rotation == 180) {
        out->base_sx = p3a_logo_w - 1; out->cx_ox = -1; out->cx_oy =  0;
        out->base_sy = p3a_logo_h - 1; out->cy_ox =  0; out->cy_oy = -1;
        out->rotated_w = p3a_logo_w;
        out->rotated_h = p3a_logo_h;
    } else if (rotation == 270) {
        out->base_sx = p3a_logo_w - 1; out->cx_ox =  0; out->cx_oy = -1;
        out->base_sy = 0;              out->cy_ox =  1; out->cy_oy =  0;
        out->rotated_w = p3a_logo_h;
        out->rotated_h = p3a_logo_w;
    } else {
        out->base_sx = 0; out->cx_ox = 1; out->cx_oy = 0;
        out->base_sy = 0; out->cy_ox = 0; out->cy_oy = 1;
        out->rotated_w = p3a_logo_w;
        out->rotated_h = p3a_logo_h;
    }
}

uint32_t intro_anim_hash3(uint32_t seed, uint32_t a, uint32_t b)
{
    uint32_t x = seed ^ (a * 0x9E3779B1u) ^ (b * 0x85EBCA77u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float intro_anim_smoothstep(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
