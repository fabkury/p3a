// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file display_ppa_upscaler.c
 * @brief PPA hardware-accelerated upscaling for Giphy channel content.
 *
 * Uses the ESP32-P4 Pixel Processing Accelerator (PPA) SRM engine for bilinear
 * interpolation upscaling with rotation and R<->B channel swap.  Border regions
 * are filled with the background color via PPA Fill.
 */

#include "sdkconfig.h"

#if CONFIG_P3A_PPA_UPSCALE_ENABLE

#include "display_ppa_upscaler.h"
#include "config_store.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ppa_upscale";

// PPA client handles (lazily initialized)
static ppa_client_handle_t s_srm_client = NULL;
static ppa_client_handle_t s_fill_client = NULL;
static bool s_initialized = false;

#define BYTES_PER_PIXEL 3  // RGB888

/**
 * @brief Lazily initialize PPA SRM and Fill clients.
 */
static esp_err_t ensure_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ppa_client_config_t srm_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&srm_cfg, &s_srm_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register PPA SRM client: %s", esp_err_to_name(err));
        return err;
    }

    ppa_client_config_t fill_cfg = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    err = ppa_register_client(&fill_cfg, &s_fill_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register PPA Fill client: %s", esp_err_to_name(err));
        ppa_unregister_client(s_srm_client);
        s_srm_client = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "PPA upscaler initialized (SRM + Fill clients)");
    return ESP_OK;
}

/**
 * @brief Map display_rotation_t to ppa_srm_rotation_angle_t.
 *
 * PPA rotation is counter-clockwise.  Our display_rotation_t values represent
 * the desired screen orientation.  When displaying a frame that was decoded
 * upright, the PPA must rotate it CCW by the same angle.
 */
static ppa_srm_rotation_angle_t map_rotation(display_rotation_t rot)
{
    switch (rot) {
        case DISPLAY_ROTATION_90:  return PPA_SRM_ROTATION_ANGLE_90;
        case DISPLAY_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
        case DISPLAY_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
        default:                   return PPA_SRM_ROTATION_ANGLE_0;
    }
}

/**
 * @brief Fill a rectangular strip in the destination buffer with the background color.
 *
 * @param dst_buffer  Full destination framebuffer pointer
 * @param dst_w       Full framebuffer width  (pixels)
 * @param dst_h       Full framebuffer height (pixels)
 * @param dst_buf_size Full framebuffer size  (bytes)
 * @param x           Strip left offset (pixels)
 * @param y           Strip top offset  (pixels)
 * @param w           Strip width  (pixels)
 * @param h           Strip height (pixels)
 * @param bg_r        Background R (in BGR memory order for the display)
 * @param bg_g        Background G
 * @param bg_b        Background B (in BGR memory order for the display)
 */
static esp_err_t fill_strip(uint8_t *dst_buffer, int dst_w, int dst_h, uint32_t dst_buf_size,
                            int x, int y, int w, int h,
                            uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    ppa_fill_oper_config_t fill_cfg = {
        .out = {
            .buffer = dst_buffer,
            .buffer_size = dst_buf_size,
            .pic_w = (uint32_t)dst_w,
            .pic_h = (uint32_t)dst_h,
            .block_offset_x = (uint32_t)x,
            .block_offset_y = (uint32_t)y,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB888,
        },
        .fill_block_w = (uint32_t)w,
        .fill_block_h = (uint32_t)h,
        .fill_argb_color = {
            // Display expects [B,G,R] in memory = PPA's native RGB888 {.b, .g, .r}
            .b = bg_b,
            .g = bg_g,
            .r = bg_r,
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_fill(s_fill_client, &fill_cfg);
}

esp_err_t display_ppa_upscale_rgb(
    const uint8_t *src_rgb, int src_w, int src_h,
    uint8_t *dst_buffer, int dst_w, int dst_h,
    bool has_borders,
    display_rotation_t rotation)
{
    if (!src_rgb || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    // For 90/270 rotation, the source effectively has swapped dimensions
    // relative to the destination.
    int eff_src_w = src_w;
    int eff_src_h = src_h;
    if (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270) {
        eff_src_w = src_h;
        eff_src_h = src_w;
    }

    // Compute uniform scale factor (fit inside, no crop)
    float scale_x = (float)dst_w / (float)eff_src_w;
    float scale_y = (float)dst_h / (float)eff_src_h;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;

    // Floor-quantize to PPA's 1/16 precision (guarantees output <= target)
    uint16_t scale_q = (uint16_t)(scale * 16.0f);  // truncate
    if (scale_q == 0) {
        scale_q = 1;  // minimum 1/16
    }
    float quantized_scale = (float)scale_q / 16.0f;

    // Actual output dimensions after quantization
    int actual_w = (eff_src_w * scale_q) / 16;
    int actual_h = (eff_src_h * scale_q) / 16;

    // Centering offsets
    int offset_x = (dst_w - actual_w) / 2;
    int offset_y = (dst_h - actual_h) / 2;

    uint32_t dst_buf_size = (uint32_t)(dst_w * dst_h * BYTES_PER_PIXEL);

    // --- Border fill (only the border strips, not the artwork region) ---
    if (has_borders && (offset_x > 0 || offset_y > 0)) {
        // Get background color (stored as R,G,B)
        uint8_t bg_r, bg_g, bg_b;
        config_store_get_background_color(&bg_r, &bg_g, &bg_b);

        // Top strip (full width)
        if (offset_y > 0) {
            err = fill_strip(dst_buffer, dst_w, dst_h, dst_buf_size,
                             0, 0, dst_w, offset_y,
                             bg_r, bg_g, bg_b);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Fill top strip failed: %s", esp_err_to_name(err));
                return err;
            }
        }

        // Bottom strip (full width)
        int bottom_y = offset_y + actual_h;
        int bottom_h = dst_h - bottom_y;
        if (bottom_h > 0) {
            err = fill_strip(dst_buffer, dst_w, dst_h, dst_buf_size,
                             0, bottom_y, dst_w, bottom_h,
                             bg_r, bg_g, bg_b);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Fill bottom strip failed: %s", esp_err_to_name(err));
                return err;
            }
        }

        // Left strip (artwork height only)
        if (offset_x > 0) {
            err = fill_strip(dst_buffer, dst_w, dst_h, dst_buf_size,
                             0, offset_y, offset_x, actual_h,
                             bg_r, bg_g, bg_b);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Fill left strip failed: %s", esp_err_to_name(err));
                return err;
            }
        }

        // Right strip (artwork height only)
        int right_x = offset_x + actual_w;
        int right_w = dst_w - right_x;
        if (right_w > 0) {
            err = fill_strip(dst_buffer, dst_w, dst_h, dst_buf_size,
                             right_x, offset_y, right_w, actual_h,
                             bg_r, bg_g, bg_b);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Fill right strip failed: %s", esp_err_to_name(err));
                return err;
            }
        }
    }

    // --- PPA SRM operation ---
    // Note: manual cache sync removed -- the PPA driver handles cache coherency
    // internally (source flush with UNALIGNED flag, destination invalidation).
    ppa_srm_oper_config_t srm_cfg = {
        .in = {
            .buffer = (const void *)src_rgb,
            .pic_w = (uint32_t)src_w,
            .pic_h = (uint32_t)src_h,
            .block_w = (uint32_t)src_w,
            .block_h = (uint32_t)src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = dst_buffer,
            .buffer_size = dst_buf_size,
            .pic_w = (uint32_t)dst_w,
            .pic_h = (uint32_t)dst_h,
            .block_offset_x = (uint32_t)offset_x,
            .block_offset_y = (uint32_t)offset_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = map_rotation(rotation),
        .scale_x = quantized_scale,
        .scale_y = quantized_scale,
        .rgb_swap = true,   // Input [R,G,B] -> PPA native [B,G,R] -> display expects [B,G,R]
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    err = ppa_do_scale_rotate_mirror(s_srm_client, &srm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPA SRM failed: %s (src=%dx%d scale=%.3f rot=%d)",
                 esp_err_to_name(err), src_w, src_h, quantized_scale, (int)rotation);
        return err;
    }

    static bool s_first_srm_ok = true;
    if (s_first_srm_ok) {
        ESP_LOGI(TAG, "PPA SRM ok (src=%dx%d -> %dx%d @ offset %d,%d scale=%.3f rot=%d)",
                 src_w, src_h, actual_w, actual_h,
                 offset_x, offset_y, quantized_scale, (int)rotation);
        s_first_srm_ok = false;
    }

    return ESP_OK;
}

void display_ppa_upscale_deinit(void)
{
    if (s_srm_client) {
        ppa_unregister_client(s_srm_client);
        s_srm_client = NULL;
    }
    if (s_fill_client) {
        ppa_unregister_client(s_fill_client);
        s_fill_client = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "PPA upscaler deinitialized");
}

#endif // CONFIG_P3A_PPA_UPSCALE_ENABLE
