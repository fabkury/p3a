#ifndef P3A_LOGO_H
#define P3A_LOGO_H

#include <stdint.h>

#define P3A_LOGO_SCALE 2

extern const int p3a_logo_w;
extern const int p3a_logo_h;
extern const int p3a_logo_scaled_w;
extern const int p3a_logo_scaled_h;

/**
 * Blit the p3a_logo image to a destination RGB888 buffer.
 *
 * @param dst              Pointer to destination buffer (RGB888 format)
 * @param dst_w            Width of destination buffer in pixels
 * @param dst_h            Height of destination buffer in pixels
 * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)
 * @param x                X position to blit to (can be negative for clipping)
 * @param y                Y position to blit to (can be negative for clipping)
 */
void p3a_logo_blit_rgb888(
    uint8_t *dst,
    int dst_w,
    int dst_h,
    int dst_stride_bytes,
    int x,
    int y
);

/**
 * Blit the p3a_logo image scaled 2x to a destination RGB888 buffer.
 *
 * @param dst              Pointer to destination buffer (RGB888 format)
 * @param dst_w            Width of destination buffer in pixels
 * @param dst_h            Height of destination buffer in pixels
 * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)
 * @param x                X position to blit to (can be negative for clipping)
 * @param y                Y position to blit to (can be negative for clipping)
 */
void p3a_logo_blit_rgb888_2x(
    uint8_t *dst,
    int dst_w,
    int dst_h,
    int dst_stride_bytes,
    int x,
    int y
);

/**
 * Blit the p3a_logo image with alpha blending to a destination RGB888 buffer.
 *
 * Composites the image over a specified background color using the given alpha.
 * Does not read from destination; writes fully opaque blended pixels.
 * Supports optional scaling (scale=0 or scale=1 means no scaling).
 *
 * @param dst              Pointer to destination buffer (RGB888 format)
 * @param dst_w            Width of destination buffer in pixels
 * @param dst_h            Height of destination buffer in pixels
 * @param dst_stride_bytes Stride of destination buffer in bytes (usually dst_w * 3)
 * @param x                X position to blit to (can be negative for clipping)
 * @param y                Y position to blit to (can be negative for clipping)
 * @param alpha            Alpha value (0=fully transparent, 255=fully opaque)
 * @param bg_r             Background red component (0-255)
 * @param bg_g             Background green component (0-255)
 * @param bg_b             Background blue component (0-255)
 * @param scale            Scale factor (0 or 1 = no scaling, 2-16 = scale up)
 */
void p3a_logo_blit_rgb888_alpha(
    uint8_t *dst,
    int dst_w,
    int dst_h,
    int dst_stride_bytes,
    int x,
    int y,
    uint8_t alpha,
    uint8_t bg_r,
    uint8_t bg_g,
    uint8_t bg_b,
    int scale
);

#endif /* P3A_LOGO_H */
