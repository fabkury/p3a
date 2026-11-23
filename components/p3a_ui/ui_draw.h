// SPDX-License-Identifier: Apache-2.0
// What the screens look like. Pure: draws on a canvas with a font, knows
// nothing of the display; host-tested (tests/host/ui/test_ui_draw.c).
// Private to the UI.
//
// Every screen that is words (a notice, setup, information, an update in
// progress, a fatal error) is one layout: a title, the text (paragraphs and
// rows of label and value, see p3a_ui_content_t) and, when there is one, a
// progress bar, centred as a group; and a hint at the bottom edge. A notice over a playing artwork is a
// band along the bottom edge. The look is set by the few numbers in
// ui_style_t, so that it can be tuned in one place.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "p3a_ui_hooks.h"
#include "ui_canvas.h"
#include "ui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const ui_font_t *font;
    int32_t title_scale, text_scale, banner_scale;  // whole multiples of the font's own size
    int32_t margin;          // from the screen's edges to the text column
    ui_colour_t background;  // of a screen that covers the artwork
    ui_colour_t title, text, accent;  // accent: the progress bar's fill and a banner's edge
    ui_colour_t label;                // a row's label, and the hint
} ui_style_t;

// A style for a screen of the given size (as the viewer sees it) with the
// built-in proportions.
void ui_style_default(ui_style_t *style, const ui_font_t *font, int32_t view_width);

// Draws a whole screen: background, title, text, progress. NULL content
// draws the background alone.
void ui_draw_screen(const ui_canvas_t *canvas, const ui_style_t *style, const p3a_ui_content_t *content);

// Where button `index` of a screen is, in viewer coordinates. False when
// the content has no such button.
bool ui_draw_button_rect(const ui_style_t *style, int32_t view_width, int32_t view_height,
                         const p3a_ui_content_t *content, int index, int32_t *x, int32_t *y, int32_t *w, int32_t *h);

// Which enabled button a tap at (tap_x, tap_y) is on, or -1.
int ui_draw_button_at(const ui_style_t *style, int32_t view_width, int32_t view_height, const p3a_ui_content_t *content,
                      int32_t tap_x, int32_t tap_y);

// Draws a notice as a band along the bottom of the screen, over whatever is
// in the canvas. Returns the band's height.
int32_t ui_draw_banner(const ui_canvas_t *canvas, const ui_style_t *style, const p3a_ui_content_t *content);

#ifdef __cplusplus
}
#endif
