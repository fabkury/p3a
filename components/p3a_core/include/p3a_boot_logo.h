// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_boot_logo.h
 * @brief Boot logo display manager with fade-in animation
 *
 * Displays the p3a logo with a smooth fade-in effect. All timings below are
 * relative to the FIRST rendered logo frame, not to boot / power-on:
 * - 0.00 to 0.25 s: Background color only (no logo)
 * - 0.25 to 2.25 s: Fade in from 0% to 100% opacity (smoothstep curve)
 * - 2.25 to 3.25 s: Hold at full opacity
 * - After 3.25 s: Release screen for normal rendering
 *
 * The boot logo is non-blocking - all other boot operations proceed in parallel.
 * While active, the logo has exclusive control of rendering (nothing else draws).
 */

#ifndef P3A_BOOT_LOGO_H
#define P3A_BOOT_LOGO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Duration of initial delay phase (background only) in milliseconds */
#define P3A_BOOT_LOGO_DELAY_MS     250

/** Duration of fade-in phase in milliseconds */
#define P3A_BOOT_LOGO_FADE_IN_MS   2000

/** Duration of full opacity hold phase in milliseconds */
#define P3A_BOOT_LOGO_HOLD_MS      1000

/** Total boot logo duration (delay + fade-in + hold) */
#define P3A_BOOT_LOGO_TOTAL_MS     (P3A_BOOT_LOGO_DELAY_MS + P3A_BOOT_LOGO_FADE_IN_MS + P3A_BOOT_LOGO_HOLD_MS)

/** Target frame duration during logo display (25 FPS) */
#define P3A_BOOT_LOGO_FRAME_MS     40

/**
 * @brief Initialize boot logo manager
 *
 * Arms the boot logo state. The display timer is NOT started here — it starts
 * on the first p3a_boot_logo_render() call (the first frame the render pipeline
 * draws), so the logo always plays its full duration regardless of how long
 * boot took to reach that first render. Should be called after
 * p3a_board_display_init() and before animation_player_init().
 *
 * @return ESP_OK on success
 */
esp_err_t p3a_boot_logo_init(void);

/**
 * @brief Check if boot logo display period is still active
 *
 * @return true before the first render, or while still within
 *         P3A_BOOT_LOGO_TOTAL_MS of the first render
 */
bool p3a_boot_logo_is_active(void);

/**
 * @brief Render boot logo to buffer
 *
 * Renders the logo with appropriate opacity based on elapsed time:
 * - During fade-in: Uses alpha blending with smoothstep interpolation
 * - During hold: Renders at full opacity without alpha blending
 *
 * @param buffer      Destination RGB888 buffer
 * @param width       Buffer width in pixels
 * @param height      Buffer height in pixels
 * @param stride      Row stride in bytes
 * @return Frame delay in ms (P3A_BOOT_LOGO_FRAME_MS), or -1 if logo period expired
 */
int p3a_boot_logo_render(uint8_t *buffer, int width, int height, size_t stride);

#ifdef __cplusplus
}
#endif

#endif /* P3A_BOOT_LOGO_H */

