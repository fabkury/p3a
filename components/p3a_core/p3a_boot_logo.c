/**
 * @file p3a_boot_logo.c
 * @brief Boot logo display manager implementation
 */

#include "p3a_boot_logo.h"
#include "p3a_logo.h"
#include "config_store.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "p3a_boot_logo";

static struct {
    int64_t start_time_us;
    bool initialized;
    bool skipped;
} s_boot_logo = {0};

/**
 * @brief Compute smoothstep interpolation
 *
 * Returns smooth Hermite interpolation between 0 and 1 when t is in [0, 1].
 * Formula: t² × (3 - 2t)
 *
 * @param t Normalized time in range [0.0, 1.0]
 * @return Smoothstep value in range [0.0, 1.0]
 */
static float smoothstep(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

esp_err_t p3a_boot_logo_init(void)
{
    s_boot_logo.start_time_us = esp_timer_get_time();
    s_boot_logo.initialized = true;
    s_boot_logo.skipped = false;

    ESP_LOGI(TAG, "Boot logo initialized: fade-in %dms, hold %dms, total %dms",
             P3A_BOOT_LOGO_FADE_IN_MS, P3A_BOOT_LOGO_HOLD_MS, P3A_BOOT_LOGO_TOTAL_MS);

    return ESP_OK;
}

bool p3a_boot_logo_is_active(void)
{
    if (!s_boot_logo.initialized || s_boot_logo.skipped) {
        return false;
    }

    int64_t elapsed_us = esp_timer_get_time() - s_boot_logo.start_time_us;
    return elapsed_us < ((int64_t)P3A_BOOT_LOGO_TOTAL_MS * 1000);
}

uint32_t p3a_boot_logo_remaining_ms(void)
{
    if (!s_boot_logo.initialized || s_boot_logo.skipped) {
        return 0;
    }

    int64_t elapsed_us = esp_timer_get_time() - s_boot_logo.start_time_us;
    int64_t remaining_us = ((int64_t)P3A_BOOT_LOGO_TOTAL_MS * 1000) - elapsed_us;

    return (remaining_us > 0) ? (uint32_t)(remaining_us / 1000) : 0;
}

int p3a_boot_logo_render(uint8_t *buffer, int width, int height, size_t stride)
{
    if (!buffer) {
        return -1;
    }

    if (!p3a_boot_logo_is_active()) {
        return -1;  // Signal caller to use normal rendering
    }

    // Calculate elapsed time
    int64_t elapsed_us = esp_timer_get_time() - s_boot_logo.start_time_us;
    int64_t elapsed_ms = elapsed_us / 1000;

    // Get background color from global settings
    uint8_t bg_r, bg_g, bg_b;
    config_store_get_background_color(&bg_r, &bg_g, &bg_b);

    // Clear buffer to background color (BGR888 format)
    for (int y = 0; y < height; y++) {
        uint8_t *row = buffer + y * stride;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = bg_b;
            row[x * 3 + 1] = bg_g;
            row[x * 3 + 2] = bg_r;
        }
    }

    // Calculate centered position for scaled logo
    const int scale = 3;
    const int logo_w = p3a_logo_w * scale;
    const int logo_h = p3a_logo_h * scale;
    int logo_x = (width - logo_w) / 2;
    int logo_y = (height - logo_h) / 2;

    if (elapsed_ms < P3A_BOOT_LOGO_FADE_IN_MS) {
        // Phase 1: Fade-in with smoothstep curve
        float t = (float)elapsed_ms / (float)P3A_BOOT_LOGO_FADE_IN_MS;
        float smooth_t = smoothstep(t);
        uint8_t alpha = (uint8_t)(smooth_t * 255.0f);

        // Use alpha blending with scale (BGR888 format)
        p3a_logo_blit_pixelwise_bgr888(
            buffer, width, height, stride,
            logo_x, logo_y,
            alpha,
            bg_b, bg_g, bg_r,
            scale
        );
    } else {
        // Phase 2: Full opacity hold
        p3a_logo_blit_pixelwise_bgr888(
            buffer, width, height, stride,
            logo_x, logo_y,
            255,
            bg_b, bg_g, bg_r,
            scale
        );
    }

    return P3A_BOOT_LOGO_FRAME_MS;
}

void p3a_boot_logo_skip(void)
{
    if (s_boot_logo.initialized && !s_boot_logo.skipped) {
        s_boot_logo.skipped = true;
        ESP_LOGI(TAG, "Boot logo skipped by user");
    }
}

