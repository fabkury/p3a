// SPDX-License-Identifier: Apache-2.0
// Between the UI's own files.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// The panel side (ui_display.c). Registers the UI as the display pipeline's
// UI source and its banner as an overlay.
esp_err_t ui_display_start(void);

// The touch task (ui_touch.c).
esp_err_t ui_touch_start(void);

// What is seen changed: draw again, and give the panel to the UI or back to
// the artwork as `covers_artwork` says. Safe from any task; returns at once.
void ui_display_changed(bool covers_artwork, bool banner);
