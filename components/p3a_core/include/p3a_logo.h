// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef P3A_LOGO_H
#define P3A_LOGO_H

#include <stdint.h>

extern const int p3a_logo_w;
extern const int p3a_logo_h;

/**
 * Fast blit the p3a_logo image to a destination BGR888 buffer using memcpy.
 *
 * This is the fastest blit method - uses row-by-row memcpy.
 * No scaling, no alpha blending - direct pixel copy.
 *
 * @param dst              Pointer to destination buffer (BGR888 format)
 * @param dst_w            Width of destination buffer in pixels
 * @param dst_h            Height of destination buffer in pixels
 * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)
 * @param x                X position to blit to (can be negative for clipping)
 * @param y                Y position to blit to (can be negative for clipping)
 */
void p3a_logo_blit_memcpy_bgr888(
    uint8_t *dst,
    int dst_w,
    int dst_h,
    int dst_stride_bytes,
    int x,
    int y
);

/**
 * Blit the p3a_logo image with alpha blending and scaling to a BGR888 buffer.
 *
 * Pixel-by-pixel blit with three optimized code paths:
 * - alpha = 255: Direct pixel copy (opaque)
 * - alpha = 0: Fill with background color only
 * - 0 < alpha < 255: Alpha-blend source with background color
 *
 * @param dst              Pointer to destination buffer (BGR888 format)
 * @param dst_w            Width of destination buffer in pixels
 * @param dst_h            Height of destination buffer in pixels
 * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)
 * @param x                X position to blit to (can be negative for clipping)
 * @param y                Y position to blit to (can be negative for clipping)
 * @param alpha            Alpha value (0=transparent/bg only, 255=fully opaque)
 * @param bg_b             Background blue component (0-255)
 * @param bg_g             Background green component (0-255)
 * @param bg_r             Background red component (0-255)
 * @param scale            Scale factor (1 = no scaling, 2-16 = scale up)
 */
void p3a_logo_blit_pixelwise_bgr888(
    uint8_t *dst,
    int dst_w,
    int dst_h,
    int dst_stride_bytes,
    int x,
    int y,
    uint8_t alpha,
    uint8_t bg_b,
    uint8_t bg_g,
    uint8_t bg_r,
    int scale
);

#endif /* P3A_LOGO_H */
