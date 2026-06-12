// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_boot_logo.c
 * @brief Intro-animation manager (formerly the boot logo).
 *
 * Drives the fixed three-phase boot sequence:
 *   blank-delay (P3A_BOOT_LOGO_DELAY_MS)
 *     -> intro-animation (P3A_BOOT_LOGO_FADE_IN_MS, parity reference until
 *        Phase 4 wires NVS-backed duration)
 *     -> hold (P3A_BOOT_LOGO_HOLD_MS)
 *
 * The animation itself is delegated to intro_anim_registry. Today the
 * manager always picks the first entry (smoothstep-fade); Phase 4 will add
 * random selection plus a force-override.
 */

#include "p3a_boot_logo.h"
#include "p3a_logo.h"
#include "config_store.h"
#include "intro_anim.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "p3a_boot_logo";

static struct {
    int64_t start_time_us;
    bool initialized;
    bool started;
    int  anim_idx;
} s_boot_logo = {0};

esp_err_t p3a_boot_logo_init(void)
{
    s_boot_logo.initialized = true;
    s_boot_logo.started = false;
    s_boot_logo.anim_idx = 0;   /* registry entry 0 = smoothstep-fade */

    ESP_LOGI(TAG,
        "Intro animation '%s' initialized: delay %dms, intro %dms, hold %dms, total %dms",
        intro_anim_registry[s_boot_logo.anim_idx].name,
        P3A_BOOT_LOGO_DELAY_MS, P3A_BOOT_LOGO_FADE_IN_MS, P3A_BOOT_LOGO_HOLD_MS,
        P3A_BOOT_LOGO_TOTAL_MS);

    return ESP_OK;
}

bool p3a_boot_logo_is_active(void)
{
    if (!s_boot_logo.initialized) {
        return false;
    }
    if (!s_boot_logo.started) {
        return true;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_boot_logo.start_time_us;
    return elapsed_us < ((int64_t)P3A_BOOT_LOGO_TOTAL_MS * 1000);
}

int p3a_boot_logo_render(uint8_t *buffer, int width, int height, size_t stride)
{
    if (!buffer) {
        return -1;
    }

    if (!s_boot_logo.started) {
        s_boot_logo.start_time_us = esp_timer_get_time();
        s_boot_logo.started = true;
        ESP_LOGI(TAG, "Intro animation timer started on first render");
    }

    if (!p3a_boot_logo_is_active()) {
        return -1;
    }

    int64_t elapsed_ms = (esp_timer_get_time() - s_boot_logo.start_time_us) / 1000;

    intro_anim_ctx_t ctx = {0};
    ctx.width  = width;
    ctx.height = height;
    ctx.stride = stride;
    config_store_get_background_color(&ctx.bg_r, &ctx.bg_g, &ctx.bg_b);
    ctx.rotation = config_store_get_rotation();
    ctx.logo_scale = 3;
    ctx.seed = (uint32_t)s_boot_logo.start_time_us;

    /* Centered logo position, accounting for 90/270 dimension swap. */
    int logo_w, logo_h;
    if (ctx.rotation == 90 || ctx.rotation == 270) {
        logo_w = p3a_logo_h * ctx.logo_scale;
        logo_h = p3a_logo_w * ctx.logo_scale;
    } else {
        logo_w = p3a_logo_w * ctx.logo_scale;
        logo_h = p3a_logo_h * ctx.logo_scale;
    }
    ctx.logo_x = (width  - logo_w) / 2;
    ctx.logo_y = (height - logo_h) / 2;

    const intro_anim_t *anim = &intro_anim_registry[s_boot_logo.anim_idx];

    if (elapsed_ms < P3A_BOOT_LOGO_DELAY_MS) {
        /* blank-delay: manager-owned flat bg fill. */
        intro_anim_fill_bg(buffer, &ctx);
    } else if (elapsed_ms < P3A_BOOT_LOGO_DELAY_MS + P3A_BOOT_LOGO_FADE_IN_MS) {
        float t = (float)(elapsed_ms - P3A_BOOT_LOGO_DELAY_MS) /
                  (float)P3A_BOOT_LOGO_FADE_IN_MS;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        anim->render(buffer, &ctx, t);
    } else {
        /* hold: canonical end state (animation t=1). Manager-owned by
         * convention — drawing it ourselves keeps the handoff invisible. */
        anim->render(buffer, &ctx, 1.0f);
    }

    return anim->frame_budget_ms;
}
