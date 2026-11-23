// SPDX-License-Identifier: Apache-2.0
// A framebuffer to draw on, addressed as the viewer sees the screen. Pure:
// host-tested (tests/host/ui/test_ui_canvas.c). Private to the UI.
//
// Everything the UI draws goes through ui_canvas_fill(): the canvas turns
// viewer coordinates into the panel's for the rotation in force and clips,
// so that no drawing code knows about rotation or can write outside the
// buffer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *pixels;   // 3 bytes a pixel: blue, green, red (the panel's order)
    int32_t width, height;  // of the panel, in pixels
    size_t stride;     // bytes from one row to the next
    int rotation;      // how the screen is turned: 0, 90, 180, 270
} ui_canvas_t;

typedef uint32_t ui_colour_t;  // 0xRRGGBB

// The size of the screen as the viewer sees it (the panel's, turned).
int32_t ui_canvas_view_width(const ui_canvas_t *canvas);
int32_t ui_canvas_view_height(const ui_canvas_t *canvas);

// Fills a rectangle given in viewer coordinates. Any part outside the
// screen is left out; a rectangle wholly outside, or empty, draws nothing.
void ui_canvas_fill(const ui_canvas_t *canvas, int32_t x, int32_t y, int32_t w, int32_t h, ui_colour_t colour);

// The whole screen in one colour.
void ui_canvas_clear(const ui_canvas_t *canvas, ui_colour_t colour);

// A rectangle's outline, `thickness` pixels wide, drawn inside its bounds.
void ui_canvas_frame(const ui_canvas_t *canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t thickness,
                     ui_colour_t colour);

#ifdef __cplusplus
}
#endif
