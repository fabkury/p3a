// SPDX-License-Identifier: Apache-2.0
// Small pictures: the icons the UI flashes in a corner, and a source's
// mark under its artworks. Pure; host-tested
// (tests/host/ui/test_ui_image.c). Private to the UI.
//
// A picture is a small binary file made from a PNG by tools/gen_image.py
// and compiled into the firmware:
//
//   "P3AI"  u8 version (1)  u8 reserved  u16 width  u16 height
//   width x height pixels, top row first, 4 bytes each: blue, green, red,
//   alpha.
//
// An icon goes over an artwork: ui_image_draw() draws a pixel when its
// alpha is 128 or more and mixes nothing. A picture made with the tool's
// --soft keeps its alpha and goes over one known colour:
// ui_image_draw_on() mixes each pixel with that ground.
//
// All numbers little-endian. The size is checked when the picture is
// opened. Drawn through the canvas, so a picture is upright for the viewer
// at every rotation.
//
// 1.x compiled each icon as a C array of hex bytes (4,700 lines for six
// icons) with its own copy of the blit code.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_IMAGE_MAX_SIDE 512

typedef struct {
    const uint8_t *pixels;  // NULL until opened
    int32_t width, height;
} ui_image_t;

// False when the bytes are not a picture or its size does not fit them.
bool ui_image_open(ui_image_t *image, const uint8_t *data, size_t len);

// Draws the picture with its top left corner at (x, y), in viewer
// coordinates, every pixel `scale` pixels on a side.
void ui_image_draw(const ui_canvas_t *canvas, const ui_image_t *image, int32_t x, int32_t y, int32_t scale);

// Draws the picture at its own size over `ground`, the colour already
// there: a pixel is the picture's colour and the ground's, mixed by its
// alpha. A wholly clear pixel is left alone.
void ui_image_draw_on(const ui_canvas_t *canvas, const ui_image_t *image, int32_t x, int32_t y, ui_colour_t ground);

#ifdef __cplusplus
}
#endif
