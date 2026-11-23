// SPDX-License-Identifier: Apache-2.0
// The clock overlay. See ui_clock.h. Pure.

#include "ui_clock.h"

#include <stdio.h>

void ui_clock_text(int hour, int minute, bool h24, char out[UI_CLOCK_TEXT_LEN])
{
    hour = ((hour % 24) + 24) % 24;
    minute = ((minute % 60) + 60) % 60;
    if (h24) {
        snprintf(out, UI_CLOCK_TEXT_LEN, "%02d:%02d", hour, minute);
    } else {
        const int h = hour % 12 == 0 ? 12 : hour % 12;
        snprintf(out, UI_CLOCK_TEXT_LEN, "%d:%02d", h, minute);
    }
}

void ui_draw_clock(const ui_canvas_t *canvas, const ui_font_t *font, const ui_clock_style_t *style, const char *text)
{
    if (font == NULL || font->data == NULL || style->scale <= 0) {
        return;
    }
    const int32_t vw = ui_canvas_view_width(canvas), vh = ui_canvas_view_height(canvas);
    const int32_t outline = style->outline < 0 ? 0 : style->outline;
    const int32_t w = ui_text_width(font, text, style->scale), h = font->height * style->scale;
    const int32_t margin = vw / 30 + outline;
    const int column = (int)style->place % 3, row = (int)style->place / 3 % 3;
    const int32_t x = column == 0 ? margin : column == 1 ? (vw - w) / 2 : vw - margin - w;
    const int32_t y = row == 0 ? margin : row == 1 ? (vh - h) / 2 : vh - margin - h;
    if (outline > 0) {
        ui_text_draw_grown(canvas, font, x, y, style->scale, style->outline_colour, text, outline);
    }
    ui_text_draw(canvas, font, x, y, style->scale, style->colour, text);
}
