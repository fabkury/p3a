// SPDX-License-Identifier: Apache-2.0
// The clock overlay: the time of day drawn over the artwork. Pure; the
// caller supplies the hour and minute. Host-tested
// (tests/host/ui/test_ui_clock.c). Private to the UI.
//
// Off unless the `clock_enabled` setting is on, and never shown before the
// device knows the time: a wrong clock is worse than none. Hours and
// minutes only, so that a still artwork is redrawn once a minute and not
// once a second.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_canvas.h"
#include "ui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

// Where on the screen, as the viewer sees it: three columns by three rows,
// numbered along the rows from the top left (the `clock_position` setting).
typedef enum {
    UI_CLOCK_TOP_LEFT = 0,
    UI_CLOCK_TOP,
    UI_CLOCK_TOP_RIGHT,
    UI_CLOCK_LEFT,
    UI_CLOCK_CENTRE,
    UI_CLOCK_RIGHT,
    UI_CLOCK_BOTTOM_LEFT,
    UI_CLOCK_BOTTOM,
    UI_CLOCK_BOTTOM_RIGHT,
} ui_clock_place_t;

typedef struct {
    ui_clock_place_t place;
    int32_t scale;           // whole multiples of the font's size
    ui_colour_t colour;
    ui_colour_t outline_colour;
    int32_t outline;         // pixels around every stroke, 0 for none: keeps the digits legible over any artwork
} ui_clock_style_t;

#define UI_CLOCK_TEXT_LEN 8

// "14:05", or with `h24` false "2:05" (no am or pm: the room is for digits).
void ui_clock_text(int hour, int minute, bool h24, char out[UI_CLOCK_TEXT_LEN]);

// Draws `text` at the style's place, a margin in from the screen's edges.
void ui_draw_clock(const ui_canvas_t *canvas, const ui_font_t *font, const ui_clock_style_t *style, const char *text);

#ifdef __cplusplus
}
#endif
