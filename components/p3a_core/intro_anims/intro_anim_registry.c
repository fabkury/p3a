// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "intro_anim.h"
#include "p3a_intro_anim_list.h"

void ia_smoothstep_fade_render (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_pixel_dissolve_render  (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_iris_wipe_render       (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_assemble_render        (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_scanline_reveal_render (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_bounce_drop_render     (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_wave_settle_render     (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_checker_tiles_render   (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_pixel_rain_render      (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_venetian_render        (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_glitch_settle_render   (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_typewriter_render      (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_spiral_reveal_render   (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_mosaic_shrink_render   (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_diamond_wipe_render    (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_color_emerge_render    (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_starburst_render       (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_plasma_dissolve_render (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_voronoi_shatter_render (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_hue_cycle_lock_render  (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_blinds_flip_render     (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_swirl_in_render        (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
void ia_channel_merge_render   (uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);

const intro_anim_t intro_anim_registry[] = {
    { .name = "smoothstep-fade", .frame_budget_ms = 33, .render = ia_smoothstep_fade_render },
    { .name = "pixel-dissolve",  .frame_budget_ms = 33, .render = ia_pixel_dissolve_render  },
    { .name = "iris-wipe",       .frame_budget_ms = 33, .render = ia_iris_wipe_render       },
    { .name = "assemble",        .frame_budget_ms = 33, .render = ia_assemble_render        },
    { .name = "scanline-reveal", .frame_budget_ms = 33, .render = ia_scanline_reveal_render },
    { .name = "bounce-drop",     .frame_budget_ms = 33, .render = ia_bounce_drop_render     },
    { .name = "wave-settle",     .frame_budget_ms = 33, .render = ia_wave_settle_render     },
    { .name = "checker-tiles",   .frame_budget_ms = 33, .render = ia_checker_tiles_render   },
    { .name = "pixel-rain",      .frame_budget_ms = 33, .render = ia_pixel_rain_render      },
    { .name = "venetian",        .frame_budget_ms = 33, .render = ia_venetian_render        },
    { .name = "glitch-settle",   .frame_budget_ms = 33, .render = ia_glitch_settle_render   },
    { .name = "typewriter",      .frame_budget_ms = 33, .render = ia_typewriter_render      },
    { .name = "spiral-reveal",   .frame_budget_ms = 33, .render = ia_spiral_reveal_render   },
    { .name = "mosaic-shrink",   .frame_budget_ms = 33, .render = ia_mosaic_shrink_render   },
    { .name = "diamond-wipe",    .frame_budget_ms = 33, .render = ia_diamond_wipe_render    },
    { .name = "color-emerge",    .frame_budget_ms = 33, .render = ia_color_emerge_render    },
    { .name = "starburst",       .frame_budget_ms = 33, .render = ia_starburst_render       },
    { .name = "plasma-dissolve", .frame_budget_ms = 33, .render = ia_plasma_dissolve_render },
    { .name = "voronoi-shatter", .frame_budget_ms = 33, .render = ia_voronoi_shatter_render },
    { .name = "hue-cycle-lock",  .frame_budget_ms = 33, .render = ia_hue_cycle_lock_render  },
    { .name = "blinds-flip",     .frame_budget_ms = 33, .render = ia_blinds_flip_render     },
    { .name = "swirl-in",        .frame_budget_ms = 33, .render = ia_swirl_in_render        },
    { .name = "channel-merge",   .frame_budget_ms = 33, .render = ia_channel_merge_render   },
};

const int intro_anim_count = (int)(sizeof(intro_anim_registry) / sizeof(intro_anim_registry[0]));

int p3a_intro_anim_get_count(void)
{
    return intro_anim_count;
}

const char *p3a_intro_anim_get_name(int idx)
{
    if (idx < 0 || idx >= intro_anim_count) return NULL;
    return intro_anim_registry[idx].name;
}
