// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_boot_logo.c
 * @brief Intro-animation manager (formerly the boot logo).
 *
 * Drives the fixed three-phase boot sequence:
 *   blank-delay (P3A_BOOT_LOGO_DELAY_MS)
 *     -> intro-animation (NVS-configurable, 1000..7500 ms, default 3000)
 *     -> hold (P3A_BOOT_LOGO_HOLD_MS)
 *
 * Selection (Phase 4, 2026-06-12): config_store_get_intro_anim_force() picks
 * either a forced animation by name or, when empty/"random", uniform random
 * via esp_random(). Duration comes from config_store_get_intro_anim_ms().
 *
 * Timing instrumentation: per-frame render time is sampled with
 * esp_timer_get_time(); when the intro window ends the manager logs
 * min/avg/max and the count of frames that overran their declared budget.
 * Operator-facing INFO so frame-budget compliance is visible per boot.
 */

#include "p3a_boot_logo.h"
#include "p3a_logo.h"
#include "config_store.h"
#include "intro_anim.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "p3a_boot_logo";

static struct {
    int64_t  start_time_us;
    bool     initialized;
    bool     started;
    bool     summary_logged;
    bool     finalized;        /* one-shot end-of-animation teardown done */
    int      anim_idx;
    uint32_t duration_ms;

    /* Per-frame timing */
    uint32_t frame_count;
    uint32_t budget_overrun_count;
    int64_t  total_render_us;
    int64_t  min_render_us;
    int64_t  max_render_us;
} s_boot_logo = {0};

/* One-shot teardown when the boot-animation window ends (idempotent). */
static void finalize_once(void);

static int find_animation_by_name(const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < intro_anim_count; ++i) {
        if (strcmp(intro_anim_registry[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t p3a_boot_logo_init(void)
{
    s_boot_logo.initialized = true;
    s_boot_logo.started = false;
    s_boot_logo.summary_logged = false;
    s_boot_logo.finalized = false;
    s_boot_logo.frame_count = 0;
    s_boot_logo.budget_overrun_count = 0;
    s_boot_logo.total_render_us = 0;
    s_boot_logo.min_render_us = INT64_MAX;
    s_boot_logo.max_render_us = 0;

    /* Duration: NVS-backed, clamped 1000..7500 (default 3000). */
    s_boot_logo.duration_ms = config_store_get_intro_anim_ms();

    /* Animation pick: force-override beats random. */
    char force[CONFIG_STORE_INTRO_ANIM_FORCE_MAX_LEN];
    config_store_get_intro_anim_force(force, sizeof(force));

    int idx = -1;
    bool forced = false;
    if (force[0] && strcmp(force, "random") != 0) {
        idx = find_animation_by_name(force);
        if (idx < 0) {
            ESP_LOGW(TAG, "Intro anim force='%s' not found in registry; falling back to random",
                     force);
        } else {
            forced = true;
        }
    }

    if (idx < 0) {
        /* Uniform random over the registry. esp_random() may not be fully
         * seeded this early in boot, but the bias is irrelevant for a
         * once-per-boot pick across ~21 entries. */
        idx = (int)(esp_random() % (uint32_t)intro_anim_count);
    }
    s_boot_logo.anim_idx = idx;

    ESP_LOGI(TAG,
        "Intro pick: '%s' [%d/%d] %s (delay %dms, intro %lums, hold %dms, total %lums)",
        intro_anim_registry[idx].name,
        idx + 1, intro_anim_count,
        forced ? "(forced)" : "(random)",
        P3A_BOOT_LOGO_DELAY_MS,
        (unsigned long)s_boot_logo.duration_ms,
        P3A_BOOT_LOGO_HOLD_MS,
        (unsigned long)(P3A_BOOT_LOGO_DELAY_MS + s_boot_logo.duration_ms + P3A_BOOT_LOGO_HOLD_MS));

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
    int64_t total_ms = (int64_t)P3A_BOOT_LOGO_DELAY_MS +
                       (int64_t)s_boot_logo.duration_ms +
                       (int64_t)P3A_BOOT_LOGO_HOLD_MS;
    int64_t elapsed_us = esp_timer_get_time() - s_boot_logo.start_time_us;
    if (elapsed_us < (total_ms * 1000)) {
        return true;
    }
    /* Window elapsed. The render caller gates on this function, so this is the
     * authoritative end-of-boot-animation signal — run the one-shot teardown
     * here (frees intro-anim scratch; logs timing) rather than relying on
     * p3a_boot_logo_render() being called once more (it isn't, once we return
     * false). */
    finalize_once();
    return false;
}

static void log_timing_summary(void)
{
    if (s_boot_logo.summary_logged || s_boot_logo.frame_count == 0) {
        return;
    }
    s_boot_logo.summary_logged = true;
    int64_t avg = s_boot_logo.total_render_us / (int64_t)s_boot_logo.frame_count;
    int budget_us = intro_anim_registry[s_boot_logo.anim_idx].frame_budget_ms * 1000;
    ESP_LOGI(TAG,
        "Intro '%s' timing: %lu frames, render us min/avg/max = %lld/%lld/%lld, budget %d us, overruns %lu",
        intro_anim_registry[s_boot_logo.anim_idx].name,
        (unsigned long)s_boot_logo.frame_count,
        (long long)s_boot_logo.min_render_us,
        (long long)avg,
        (long long)s_boot_logo.max_render_us,
        budget_us,
        (unsigned long)s_boot_logo.budget_overrun_count);
}

static void finalize_once(void)
{
    if (s_boot_logo.finalized) {
        return;
    }
    s_boot_logo.finalized = true;
    log_timing_summary();
    /* Free the intro-anim scratch now that the boot animation is over, so its
     * per-frame working buffers (~12 KB internal RAM for the heaviest effects)
     * aren't held for the entire firmware uptime. */
    intro_anim_scratch_release();
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
        /* is_active() ran finalize_once() on expiry (timing summary + scratch
         * free); nothing more to do here. */
        return -1;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_ms = (now_us - s_boot_logo.start_time_us) / 1000;

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

    int64_t render_start_us = esp_timer_get_time();

    if (elapsed_ms < P3A_BOOT_LOGO_DELAY_MS) {
        /* blank-delay: manager-owned flat bg fill. */
        intro_anim_fill_bg(buffer, &ctx);
    } else if (elapsed_ms < (int64_t)P3A_BOOT_LOGO_DELAY_MS + (int64_t)s_boot_logo.duration_ms) {
        float t = (float)(elapsed_ms - P3A_BOOT_LOGO_DELAY_MS) /
                  (float)s_boot_logo.duration_ms;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        anim->render(buffer, &ctx, t);
    } else {
        /* hold: canonical end state (animation t=1). Manager-owned by
         * convention — drawing it ourselves keeps the handoff invisible. */
        anim->render(buffer, &ctx, 1.0f);
    }

    int64_t render_us = esp_timer_get_time() - render_start_us;
    s_boot_logo.frame_count++;
    s_boot_logo.total_render_us += render_us;
    if (render_us < s_boot_logo.min_render_us) s_boot_logo.min_render_us = render_us;
    if (render_us > s_boot_logo.max_render_us) s_boot_logo.max_render_us = render_us;
    if (render_us > (int64_t)anim->frame_budget_ms * 1000) {
        s_boot_logo.budget_overrun_count++;
    }

    return anim->frame_budget_ms;
}
