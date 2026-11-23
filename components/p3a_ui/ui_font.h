// SPDX-License-Identifier: Apache-2.0
// Text: pixel fonts drawn at whole multiples of their own size. Pure:
// host-tested (tests/host/ui/test_ui_font.c). Private to the UI.
//
// A font is a small binary file made from a pixel font's TTF by
// tools/gen_font.py and compiled into the firmware:
//
//   "P3AF"  u8 version (1)  u8 height  u8 baseline  u8 reserved
//   u16 glyph_count
//   glyph_count x { u16 codepoint, u8 advance, u8 reserved, u32 offset }   sorted by codepoint
//   bitmaps: for each glyph, `height` rows of ceil(advance / 8) bytes,
//            most significant bit first, a set bit is ink
//
// All numbers little-endian; `offset` counts from the start of the file.
// Every size is checked when the font is opened, so drawing never reads
// outside the file whatever it contains.
//
// Text is UTF-8. A character the font lacks is drawn as its '?'.
//
// 1.x drew text with uGFX (54,000 lines) from anti-aliased conversions of
// DejaVu Sans, and a matching bug made every size fall back to one.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    uint8_t height;    // of a line of text, in the font's own pixels
    uint8_t baseline;  // rows from the top of the line to the baseline
    uint16_t count;
} ui_font_t;

// Checks a font file and prepares it for use. False when it is not a font
// or any of its sizes does not fit the file.
bool ui_font_open(ui_font_t *font, const uint8_t *data, size_t len);

// The width of one line of text at `scale`, in pixels. Stops at the end of
// the string or at a newline.
int32_t ui_text_width(const ui_font_t *font, const char *text, int32_t scale);

// Draws one line of text with its top left corner at (x, y), in viewer
// coordinates, and returns its width. Stops at a newline.
int32_t ui_text_draw(const ui_canvas_t *canvas, const ui_font_t *font, int32_t x, int32_t y, int32_t scale,
                     ui_colour_t colour, const char *text);

typedef enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTRE, UI_ALIGN_RIGHT } ui_align_t;

// Draws text in a column `width` wide starting at (x, y): broken into lines
// at spaces where it would not fit (a word wider than the column is broken
// where it must be), and at every newline; each line aligned within the
// column. `line_gap` is extra pixels between lines. At most `max_lines`
// lines are drawn. Returns the height used. With a NULL canvas nothing is
// drawn and the height is only measured.
int32_t ui_text_block(const ui_canvas_t *canvas, const ui_font_t *font, int32_t x, int32_t y, int32_t width,
                      int32_t scale, int32_t line_gap, ui_align_t align, ui_colour_t colour, const char *text,
                      int32_t max_lines);

#ifdef __cplusplus
}
#endif
