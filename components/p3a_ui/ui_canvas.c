// SPDX-License-Identifier: Apache-2.0
// A framebuffer addressed as the viewer sees the screen. See ui_canvas.h. Pure.

#include "ui_canvas.h"

int32_t ui_canvas_view_width(const ui_canvas_t *canvas)
{
    return (canvas->rotation == 90 || canvas->rotation == 270) ? canvas->height : canvas->width;
}

int32_t ui_canvas_view_height(const ui_canvas_t *canvas)
{
    return (canvas->rotation == 90 || canvas->rotation == 270) ? canvas->width : canvas->height;
}

void ui_canvas_fill(const ui_canvas_t *canvas, int32_t x, int32_t y, int32_t w, int32_t h, ui_colour_t colour)
{
    const int32_t vw = ui_canvas_view_width(canvas), vh = ui_canvas_view_height(canvas);
    // Clip in viewer coordinates, with 64-bit sums so that no argument overflows.
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > vw) {
        x1 = vw;
    }
    if (y1 > vh) {
        y1 = vh;
    }
    if (x0 >= x1 || y0 >= y1 || canvas->pixels == NULL) {
        return;
    }
    // The same rectangle on the panel. The inverse of the viewer's mapping
    // (ui_gesture_to_view): turned 90, the viewer's (x, y) is the panel's
    // (height - 1 - y, x), and so on.
    int32_t px0, py0, pw, ph;
    const int32_t cw = (int32_t)(x1 - x0), ch = (int32_t)(y1 - y0);
    switch (canvas->rotation) {
    case 90:
        px0 = canvas->width - (int32_t)y1;
        py0 = (int32_t)x0;
        pw = ch;
        ph = cw;
        break;
    case 180:
        px0 = canvas->width - (int32_t)x1;
        py0 = canvas->height - (int32_t)y1;
        pw = cw;
        ph = ch;
        break;
    case 270:
        px0 = (int32_t)y0;
        py0 = canvas->height - (int32_t)x1;
        pw = ch;
        ph = cw;
        break;
    default:
        px0 = (int32_t)x0;
        py0 = (int32_t)y0;
        pw = cw;
        ph = ch;
        break;
    }
    const uint8_t b = (uint8_t)colour, g = (uint8_t)(colour >> 8), r = (uint8_t)(colour >> 16);
    for (int32_t row = 0; row < ph; row++) {
        uint8_t *p = canvas->pixels + (size_t)(py0 + row) * canvas->stride + (size_t)px0 * 3;
        for (int32_t col = 0; col < pw; col++) {
            *p++ = b;
            *p++ = g;
            *p++ = r;
        }
    }
}

void ui_canvas_clear(const ui_canvas_t *canvas, ui_colour_t colour)
{
    ui_canvas_fill(canvas, 0, 0, ui_canvas_view_width(canvas), ui_canvas_view_height(canvas), colour);
}

void ui_canvas_frame(const ui_canvas_t *canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t thickness,
                     ui_colour_t colour)
{
    if (thickness <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (thickness * 2 >= w || thickness * 2 >= h) {
        ui_canvas_fill(canvas, x, y, w, h, colour);
        return;
    }
    ui_canvas_fill(canvas, x, y, w, thickness, colour);
    ui_canvas_fill(canvas, x, y + h - thickness, w, thickness, colour);
    ui_canvas_fill(canvas, x, y + thickness, thickness, h - 2 * thickness, colour);
    ui_canvas_fill(canvas, x + w - thickness, y + thickness, thickness, h - 2 * thickness, colour);
}
