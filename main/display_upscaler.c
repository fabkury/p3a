/**
 * @file display_upscaler.c
 * @brief Display upscaler and border fill implementation
 * 
 * Contains the parallel upscale workers and blit functions for 
 * scaling source frames to the display with rotation support.
 */

#include "display_renderer_priv.h"
#include "config_store.h"

// Diagnostic: Use only top worker to process ALL rows (glitch-free configuration)
// Set to 0 to enable two-worker parallel mode (for testing cache coherency fixes)
#define DISPLAY_UPSCALE_SINGLE_WORKER 0

// ============================================================================
// Row-based RGBA blit implementation
// ============================================================================

static void blit_upscaled_rows_rgba(const uint8_t *src_rgba, int src_w, int src_h,
                                    uint8_t *dst_buffer, int dst_w, int dst_h,
                                    int row_start, int row_end,
                                    int offset_x, int offset_y, int scaled_w, int scaled_h,
                                    const uint16_t *lookup_x, const uint16_t *lookup_y,
                                    display_rotation_t rotation)
{
    if (!src_rgba || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    if (!lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale lookup tables not initialized");
        return;
    }

    const uint32_t *src_rgba32 = (const uint32_t *)src_rgba;

#if CONFIG_P3A_USE_PIE_SIMD
    // Row duplication optimization: detect runs of consecutive dst rows that map to the same source.
    const uint16_t *run_lookup = (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270)
                                  ? lookup_x : lookup_y;
    uint16_t prev_run_key = UINT16_MAX;
    uint8_t *last_rendered_row = NULL;
#endif

    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
        if (scaled_w <= 0 || scaled_h <= 0) {
            continue;
        }
        if (dst_y < offset_y || dst_y >= (offset_y + scaled_h)) {
            continue;
        }
        const int local_y = dst_y - offset_y;

        uint8_t *dst_row_bytes = dst_buffer + (size_t)dst_y * g_display_row_stride;

#if CONFIG_P3A_USE_PIE_SIMD
        const uint16_t run_key = run_lookup[local_y];
        if (run_key == prev_run_key && last_rendered_row != NULL) {
            memcpy(dst_row_bytes, last_rendered_row, g_display_row_stride);
            continue;
        }
        prev_run_key = run_key;
        last_rendered_row = dst_row_bytes;
#endif

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)dst_row_bytes;
#else
        uint8_t *dst_row = dst_row_bytes;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[local_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint32_t rgba = src_row32[src_x];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            case DISPLAY_ROTATION_90: {
                const uint16_t src_x_fixed = lookup_x[local_y];
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_y = lookup_y[local_x];
                    const uint16_t src_y = (src_h - 1) - raw_src_y;
                    const uint32_t rgba = src_rgba32[(size_t)src_y * src_w + src_x_fixed];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            case DISPLAY_ROTATION_180: {
                const uint16_t raw_src_y = lookup_y[local_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_x = lookup_x[local_x];
                    const uint16_t src_x = (src_w - 1) - raw_src_x;
                    const uint32_t rgba = src_row32[src_x];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            case DISPLAY_ROTATION_270: {
                const uint16_t raw_src_x = lookup_x[local_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_y = lookup_y[local_x];
                    const uint32_t rgba = src_rgba32[(size_t)src_y * src_w + src_x_fixed];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            default: {
                const uint16_t src_y = lookup_y[local_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint32_t rgba = src_row32[src_x];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
        }
    }
}

// ============================================================================
// Row-based RGB blit implementation
// ============================================================================

static void blit_upscaled_rows_rgb(const uint8_t *src_rgb, int src_w, int src_h,
                                   uint8_t *dst_buffer, int dst_w, int dst_h,
                                   int row_start, int row_end,
                                   int offset_x, int offset_y, int scaled_w, int scaled_h,
                                   const uint16_t *lookup_x, const uint16_t *lookup_y,
                                   display_rotation_t rotation)
{
    if (!src_rgb || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    if (!lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale lookup tables not initialized");
        return;
    }

#if CONFIG_P3A_USE_PIE_SIMD
    const uint16_t *run_lookup = (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270)
                                  ? lookup_x : lookup_y;
    uint16_t prev_run_key = UINT16_MAX;
    uint8_t *last_rendered_row = NULL;
#endif

    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
        if (scaled_w <= 0 || scaled_h <= 0) {
            continue;
        }
        if (dst_y < offset_y || dst_y >= (offset_y + scaled_h)) {
            continue;
        }
        const int local_y = dst_y - offset_y;

        uint8_t *dst_row_bytes = dst_buffer + (size_t)dst_y * g_display_row_stride;

#if CONFIG_P3A_USE_PIE_SIMD
        const uint16_t run_key = run_lookup[local_y];
        if (run_key == prev_run_key && last_rendered_row != NULL) {
            memcpy(dst_row_bytes, last_rendered_row, g_display_row_stride);
            continue;
        }
        prev_run_key = run_key;
        last_rendered_row = dst_row_bytes;
#endif

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)dst_row_bytes;
#else
        uint8_t *dst_row = dst_row_bytes;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[local_y];
                const uint8_t *src_row = src_rgb + (size_t)src_y * (size_t)src_w * 3U;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint8_t *p = src_row + (size_t)src_x * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            case DISPLAY_ROTATION_90: {
                const uint16_t src_x_fixed = lookup_x[local_y];
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_y = lookup_y[local_x];
                    const uint16_t src_y = (src_h - 1) - raw_src_y;
                    const uint8_t *p = src_rgb + ((size_t)src_y * (size_t)src_w + (size_t)src_x_fixed) * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            case DISPLAY_ROTATION_180: {
                const uint16_t raw_src_y = lookup_y[local_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint8_t *src_row = src_rgb + (size_t)src_y * (size_t)src_w * 3U;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_x = lookup_x[local_x];
                    const uint16_t src_x = (src_w - 1) - raw_src_x;
                    const uint8_t *p = src_row + (size_t)src_x * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            case DISPLAY_ROTATION_270: {
                const uint16_t raw_src_x = lookup_x[local_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_y = lookup_y[local_x];
                    const uint8_t *p = src_rgb + ((size_t)src_y * (size_t)src_w + (size_t)src_x_fixed) * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            default: {
                const uint16_t src_y = lookup_y[local_y];
                const uint8_t *src_row = src_rgb + (size_t)src_y * (size_t)src_w * 3U;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint8_t *p = src_row + (size_t)src_x * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
        }
    }
}

// ============================================================================
// Border fill (second pass after upscale)
// ============================================================================

#if CONFIG_LCD_PIXEL_FORMAT_RGB888
static inline void fill_rgb888_pixels(uint8_t *row, int x_start, int x_end, uint8_t b, uint8_t g, uint8_t r)
{
    if (!row || x_start >= x_end) return;
    uint8_t *p = row + (size_t)x_start * 3U;
    int count = x_end - x_start;

    while (count >= 4) {
        p[0] = b; p[1] = g; p[2] = r;
        p[3] = b; p[4] = g; p[5] = r;
        p[6] = b; p[7] = g; p[8] = r;
        p[9] = b; p[10] = g; p[11] = r;
        p += 12;
        count -= 4;
    }
    while (count-- > 0) {
        p[0] = b; p[1] = g; p[2] = r;
        p += 3;
    }
}
#endif

static void fill_borders_rows(uint8_t *dst_buffer, int dst_w, int dst_h,
                              int row_start, int row_end,
                              int offset_x, int offset_y, int scaled_w, int scaled_h)
{
    if (!dst_buffer || dst_w <= 0 || dst_h <= 0) return;

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    const int img_x0 = offset_x;
    const int img_x1 = offset_x + scaled_w;
    const int img_y0 = offset_y;
    const int img_y1 = offset_y + scaled_h;

    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)dst_y * g_display_row_stride);
#else
        uint8_t *dst_row = dst_buffer + (size_t)dst_y * g_display_row_stride;
#endif

        if (dst_y < img_y0 || dst_y >= img_y1) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
            const uint16_t bg = g_upscale_bg_rgb565;
            uintptr_t addr = (uintptr_t)dst_row;
            int x = 0;
            if ((addr & 0x3U) && dst_w > 0) {
                dst_row[0] = bg;
                x = 1;
            }
            const uint32_t bg32 = ((uint32_t)bg << 16) | (uint32_t)bg;
            uint32_t *p32 = (uint32_t *)(dst_row + x);
            const int pairs = (dst_w - x) / 2;
            for (int i = 0; i < pairs; ++i) {
                p32[i] = bg32;
            }
            const int filled = x + pairs * 2;
            if (filled < dst_w) {
                dst_row[filled] = bg;
            }
#else
            fill_rgb888_pixels(dst_row, 0, dst_w, g_upscale_bg_b, g_upscale_bg_g, g_upscale_bg_r);
#endif
        } else {
            if (img_x0 > 0) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                const uint16_t bg = g_upscale_bg_rgb565;
                for (int x = 0; x < img_x0; ++x) {
                    dst_row[x] = bg;
                }
#else
                fill_rgb888_pixels(dst_row, 0, img_x0, g_upscale_bg_b, g_upscale_bg_g, g_upscale_bg_r);
#endif
            }
            if (img_x1 < dst_w) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                const uint16_t bg = g_upscale_bg_rgb565;
                for (int x = img_x1; x < dst_w; ++x) {
                    dst_row[x] = bg;
                }
#else
                fill_rgb888_pixels(dst_row, img_x1, dst_w, g_upscale_bg_b, g_upscale_bg_g, g_upscale_bg_r);
#endif
            }
        }
    }
}

// ============================================================================
// Worker tasks
// ============================================================================

void display_upscale_worker_top_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 0);

    while (true) {
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);

        DISPLAY_MEMORY_BARRIER();

        if (g_upscale_src_buffer && g_upscale_dst_buffer &&
            g_upscale_row_start_top < g_upscale_row_end_top) {
            if (g_upscale_src_bpp == 3) {
                blit_upscaled_rows_rgb(g_upscale_src_buffer,
                                       g_upscale_src_w, g_upscale_src_h,
                                       g_upscale_dst_buffer,
                                       EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                       g_upscale_row_start_top, g_upscale_row_end_top,
                                       g_upscale_offset_x, g_upscale_offset_y,
                                       g_upscale_scaled_w, g_upscale_scaled_h,
                                       g_upscale_lookup_x, g_upscale_lookup_y,
                                       g_upscale_rotation);
            } else {
                blit_upscaled_rows_rgba(g_upscale_src_buffer,
                                        g_upscale_src_w, g_upscale_src_h,
                                        g_upscale_dst_buffer,
                                        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                        g_upscale_row_start_top, g_upscale_row_end_top,
                                        g_upscale_offset_x, g_upscale_offset_y,
                                        g_upscale_scaled_w, g_upscale_scaled_h,
                                        g_upscale_lookup_x, g_upscale_lookup_y,
                                        g_upscale_rotation);
            }
            if (g_upscale_has_borders) {
                fill_borders_rows(g_upscale_dst_buffer,
                                  EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                  g_upscale_row_start_top, g_upscale_row_end_top,
                                  g_upscale_offset_x, g_upscale_offset_y,
                                  g_upscale_scaled_w, g_upscale_scaled_h);
            }
        }

        DISPLAY_MEMORY_BARRIER();

        g_upscale_worker_top_done = true;
        if (g_upscale_main_task) {
            xTaskNotify(g_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

void display_upscale_worker_bottom_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 1);

    while (true) {
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);

        DISPLAY_MEMORY_BARRIER();

        if (g_upscale_src_buffer && g_upscale_dst_buffer &&
            g_upscale_row_start_bottom < g_upscale_row_end_bottom) {
            if (g_upscale_src_bpp == 3) {
                blit_upscaled_rows_rgb(g_upscale_src_buffer,
                                       g_upscale_src_w, g_upscale_src_h,
                                       g_upscale_dst_buffer,
                                       EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                       g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                                       g_upscale_offset_x, g_upscale_offset_y,
                                       g_upscale_scaled_w, g_upscale_scaled_h,
                                       g_upscale_lookup_x, g_upscale_lookup_y,
                                       g_upscale_rotation);
            } else {
                blit_upscaled_rows_rgba(g_upscale_src_buffer,
                                        g_upscale_src_w, g_upscale_src_h,
                                        g_upscale_dst_buffer,
                                        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                        g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                                        g_upscale_offset_x, g_upscale_offset_y,
                                        g_upscale_scaled_w, g_upscale_scaled_h,
                                        g_upscale_lookup_x, g_upscale_lookup_y,
                                        g_upscale_rotation);
            }
            if (g_upscale_has_borders) {
                fill_borders_rows(g_upscale_dst_buffer,
                                  EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                  g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                                  g_upscale_offset_x, g_upscale_offset_y,
                                  g_upscale_scaled_w, g_upscale_scaled_h);
            }
        }

        DISPLAY_MEMORY_BARRIER();

        g_upscale_worker_bottom_done = true;
        if (g_upscale_main_task) {
            xTaskNotify(g_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

// ============================================================================
// Public parallel upscale API
// ============================================================================

void display_renderer_parallel_upscale(const uint8_t *src_rgba, int src_w, int src_h,
                                       uint8_t *dst_buffer,
                                       const uint16_t *lookup_x, const uint16_t *lookup_y,
                                       int offset_x, int offset_y, int scaled_w, int scaled_h,
                                       bool has_borders,
                                       display_rotation_t rotation)
{
    const int dst_h = EXAMPLE_LCD_V_RES;
    const int dst_w = EXAMPLE_LCD_H_RES;

    if (!src_rgba || !dst_buffer || !lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale: NULL pointer (src=%p dst=%p lx=%p ly=%p)",
                 src_rgba, dst_buffer, lookup_x, lookup_y);
        return;
    }
    if (src_w <= 0 || src_h <= 0 || scaled_w <= 0 || scaled_h <= 0) {
        ESP_LOGE(DISPLAY_TAG, "Upscale: Invalid dimensions (src %dx%d, scaled %dx%d)",
                 src_w, src_h, scaled_w, scaled_h);
        return;
    }
    if (offset_x < 0 || offset_y < 0 || offset_x + scaled_w > dst_w || offset_y + scaled_h > dst_h) {
        ESP_LOGE(DISPLAY_TAG, "Upscale: Offset/scaled out of bounds (offset %d,%d scaled %dx%d dst %dx%d)",
                 offset_x, offset_y, scaled_w, scaled_h, dst_w, dst_h);
        return;
    }

#if !DISPLAY_UPSCALE_SINGLE_WORKER
    const int mid_row = dst_h / 2;
#endif

    g_upscale_src_buffer = src_rgba;
    g_upscale_src_bpp = 4;
    g_upscale_dst_buffer = dst_buffer;
    g_upscale_lookup_x = lookup_x;
    g_upscale_lookup_y = lookup_y;
    g_upscale_src_w = src_w;
    g_upscale_src_h = src_h;
    g_upscale_rotation = rotation;
    g_upscale_offset_x = offset_x;
    g_upscale_offset_y = offset_y;
    g_upscale_scaled_w = scaled_w;
    g_upscale_scaled_h = scaled_h;
    g_upscale_has_borders = has_borders;
    config_store_get_background_color(&g_upscale_bg_r, &g_upscale_bg_g, &g_upscale_bg_b);
    g_upscale_bg_rgb565 = rgb565(g_upscale_bg_r, g_upscale_bg_g, g_upscale_bg_b);
    g_upscale_main_task = xTaskGetCurrentTaskHandle();

    g_upscale_worker_top_done = false;
    g_upscale_worker_bottom_done = false;

#if DISPLAY_UPSCALE_SINGLE_WORKER
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = dst_h;
    g_upscale_row_start_bottom = dst_h;
    g_upscale_row_end_bottom = dst_h;
#else
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = mid_row;
    g_upscale_row_start_bottom = mid_row;
    g_upscale_row_end_bottom = dst_h;
#endif

    DISPLAY_MEMORY_BARRIER();

    if (g_upscale_worker_top && g_upscale_worker_bottom) {
        xTaskNotify(g_upscale_worker_top, 1, eSetBits);
        xTaskNotify(g_upscale_worker_bottom, 1, eSetBits);
    }

    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;

    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            taskYIELD();
        }
    }

    if (!g_upscale_worker_top_done || !g_upscale_worker_bottom_done) {
        ESP_LOGW(DISPLAY_TAG, "Upscale workers may not have completed properly");
    }

    DISPLAY_MEMORY_BARRIER();
}

void display_renderer_parallel_upscale_rgb(const uint8_t *src_rgb, int src_w, int src_h,
                                          uint8_t *dst_buffer,
                                          const uint16_t *lookup_x, const uint16_t *lookup_y,
                                          int offset_x, int offset_y, int scaled_w, int scaled_h,
                                          bool has_borders,
                                          display_rotation_t rotation)
{
    const int dst_h = EXAMPLE_LCD_V_RES;
    const int dst_w = EXAMPLE_LCD_H_RES;

    if (!src_rgb || !dst_buffer || !lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale RGB: NULL pointer (src=%p dst=%p lx=%p ly=%p)",
                 src_rgb, dst_buffer, lookup_x, lookup_y);
        return;
    }
    if (src_w <= 0 || src_h <= 0 || scaled_w <= 0 || scaled_h <= 0) {
        ESP_LOGE(DISPLAY_TAG, "Upscale RGB: Invalid dimensions (src %dx%d, scaled %dx%d)",
                 src_w, src_h, scaled_w, scaled_h);
        return;
    }
    if (offset_x < 0 || offset_y < 0 || offset_x + scaled_w > dst_w || offset_y + scaled_h > dst_h) {
        ESP_LOGE(DISPLAY_TAG, "Upscale RGB: Offset/scaled out of bounds (offset %d,%d scaled %dx%d dst %dx%d)",
                 offset_x, offset_y, scaled_w, scaled_h, dst_w, dst_h);
        return;
    }

#if !DISPLAY_UPSCALE_SINGLE_WORKER
    const int mid_row = dst_h / 2;
#endif

    g_upscale_src_buffer = src_rgb;
    g_upscale_src_bpp = 3;
    g_upscale_dst_buffer = dst_buffer;
    g_upscale_lookup_x = lookup_x;
    g_upscale_lookup_y = lookup_y;
    g_upscale_src_w = src_w;
    g_upscale_src_h = src_h;
    g_upscale_rotation = rotation;
    g_upscale_offset_x = offset_x;
    g_upscale_offset_y = offset_y;
    g_upscale_scaled_w = scaled_w;
    g_upscale_scaled_h = scaled_h;
    g_upscale_has_borders = has_borders;
    config_store_get_background_color(&g_upscale_bg_r, &g_upscale_bg_g, &g_upscale_bg_b);
    g_upscale_bg_rgb565 = rgb565(g_upscale_bg_r, g_upscale_bg_g, g_upscale_bg_b);
    g_upscale_main_task = xTaskGetCurrentTaskHandle();

    g_upscale_worker_top_done = false;
    g_upscale_worker_bottom_done = false;

#if DISPLAY_UPSCALE_SINGLE_WORKER
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = dst_h;
    g_upscale_row_start_bottom = dst_h;
    g_upscale_row_end_bottom = dst_h;
#else
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = mid_row;
    g_upscale_row_start_bottom = mid_row;
    g_upscale_row_end_bottom = dst_h;
#endif

    DISPLAY_MEMORY_BARRIER();

    if (g_upscale_worker_top && g_upscale_worker_bottom) {
        xTaskNotify(g_upscale_worker_top, 1, eSetBits);
        xTaskNotify(g_upscale_worker_bottom, 1, eSetBits);
    }

    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;

    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            taskYIELD();
        }
    }

    if (!g_upscale_worker_top_done || !g_upscale_worker_bottom_done) {
        ESP_LOGW(DISPLAY_TAG, "Upscale workers may not have completed properly");
    }

    DISPLAY_MEMORY_BARRIER();
}

