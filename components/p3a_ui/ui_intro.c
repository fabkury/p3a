// SPDX-License-Identifier: Apache-2.0
// The boot animation: one of the intro animations (intro/), chosen at
// boot, drawn while the INTRO screen is up. See ui_internal.h.
//
// Three phases, timed from the first frame actually drawn (not from power
// on, so that a slow boot does not eat the animation):
//
//   250 ms       the background colour alone
//   the intro    `intro_duration_ms` (1 to 7.5 s, default 3 s): the animation, t from 0 to 1
//   1000 ms      the finished logo, held
//
// The animations are pure functions of (buffer, context, t), checked on the
// workstation by host/intro-anim-lab; this file owns the clock, the choice
// and the bookends. It is 1.x's p3a_boot_logo.c on 2.0's settings.

#include <string.h>

#include "display_renderer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "intro/intro_anim.h"
#include "intro/p3a_logo.h"
#include "p3a_board.h"
#include "p3a_settings.h"
#include "ui_internal.h"

static const char *TAG = "ui_intro";

#define DELAY_MS   250
#define HOLD_MS    1000
#define LOGO_SCALE 3

static struct {
    bool armed;        // chosen and not yet over
    bool started;      // the first frame was drawn
    int index;
    uint32_t duration_ms;
    int64_t start_us;
    // How the frames went, for one log line at the end.
    uint32_t frames, overruns;
    int64_t total_us, max_us;
} s;

bool ui_intro_arm(void)
{
    static p3a_settings_t settings;  // too large for a stack; boot only
    p3a_settings_get(&settings);
    memset(&s, 0, sizeof(s));
    s.duration_ms = (uint32_t)settings.intro_duration_ms;
    int index = -1;
    const char *wanted = settings.intro_animation;
    if (wanted[0] != '\0') {
        for (int i = 0; i < intro_anim_count; i++) {
            if (strcmp(intro_anim_registry[i].name, wanted) == 0) {
                index = i;
            }
        }
        if (index < 0) {
            ESP_LOGW(TAG, "no boot animation is called '%s'; one is picked at random", wanted);
        }
    }
    const bool chosen = index >= 0;
    if (!chosen) {
        index = (int)(esp_random() % (uint32_t)intro_anim_count);
    }
    s.index = index;
    s.armed = true;
    ESP_LOGI(TAG, "boot animation '%s' (%s), %u ms", intro_anim_registry[index].name, chosen ? "set" : "random",
             (unsigned)s.duration_ms);
    return true;
}

int ui_intro_render(uint8_t *buffer)
{
    if (!s.armed) {
        return -1;
    }
    const int64_t now_us = esp_timer_get_time();
    if (!s.started) {
        s.started = true;
        s.start_us = now_us;
    }
    const int64_t elapsed_ms = (now_us - s.start_us) / 1000;
    const intro_anim_t *anim = &intro_anim_registry[s.index];
    if (elapsed_ms >= (int64_t)DELAY_MS + s.duration_ms + HOLD_MS) {
        s.armed = false;
        intro_anim_scratch_release();
        ESP_LOGI(TAG, "boot animation '%s': %u frames, average %lld us, slowest %lld us, %u over the %d ms budget",
                 anim->name, (unsigned)s.frames, (long long)(s.frames ? s.total_us / s.frames : 0), (long long)s.max_us,
                 (unsigned)s.overruns, anim->frame_budget_ms);
        return -1;
    }

    intro_anim_ctx_t ctx = {0};
    ctx.width = P3A_DISPLAY_WIDTH;
    ctx.height = P3A_DISPLAY_HEIGHT;
    ctx.stride = display_renderer_row_stride();
    display_renderer_get_background(&ctx.bg_r, &ctx.bg_g, &ctx.bg_b);
    ctx.rotation = (uint16_t)display_renderer_get_rotation();
    ctx.logo_scale = LOGO_SCALE;
    ctx.seed = (uint32_t)s.start_us;
    const bool sideways = ctx.rotation == 90 || ctx.rotation == 270;
    ctx.logo_x = (ctx.width - (sideways ? p3a_logo_h : p3a_logo_w) * LOGO_SCALE) / 2;
    ctx.logo_y = (ctx.height - (sideways ? p3a_logo_w : p3a_logo_h) * LOGO_SCALE) / 2;

    if (elapsed_ms < DELAY_MS) {
        intro_anim_fill_bg(buffer, &ctx);
    } else {
        float t = (float)(elapsed_ms - DELAY_MS) / (float)s.duration_ms;
        anim->render(buffer, &ctx, t > 1.0f ? 1.0f : t);  // past the end: the finished logo, held
    }
    const int64_t took_us = esp_timer_get_time() - now_us;
    s.frames++;
    s.total_us += took_us;
    if (took_us > s.max_us) {
        s.max_us = took_us;
    }
    if (took_us > (int64_t)anim->frame_budget_ms * 1000) {
        s.overruns++;
    }
    return anim->frame_budget_ms;
}
