// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#ifndef INTRO_ANIM_LAB_HARNESS_H
#define INTRO_ANIM_LAB_HARNESS_H

#include <stdint.h>
#include "intro_anim.h"

/* Canvas the host renders into: matches the device (720x720 BGR888). */
#define HARNESS_W      720
#define HARNESS_H      720
#define HARNESS_STRIDE (HARNESS_W * 3)
#define HARNESS_BUFSZ  ((size_t)HARNESS_STRIDE * HARNESS_H)

/* Bookend timings, hardcoded to match the device manager. */
#define HARNESS_BLANK_DELAY_MS 250
#define HARNESS_HOLD_MS        1000

/* Default intro-animation duration (matches the planned NVS default). */
#define HARNESS_DEFAULT_INTRO_MS 3000

/* Fill (logo_x, logo_y) using the same centering math the device uses. */
void harness_compute_logo_position(intro_anim_ctx_t *ctx);

/* Background presets cycled by the viewer's `B` key. */
typedef struct { const char *name; uint8_t r, g, b; } harness_bg_preset_t;
extern const harness_bg_preset_t harness_bg_presets[];
extern const int harness_bg_preset_count;

/* Subcommand entry points (used by main.c). */
int harness_viewer_run(void);
int harness_dump_run(int argc, char **argv);
int harness_check_run(void);

#endif
