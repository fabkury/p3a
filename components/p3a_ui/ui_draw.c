// SPDX-License-Identifier: Apache-2.0
// What the screens look like. See ui_draw.h. Pure.

#include "ui_draw.h"

#define MAX_LINES 8

void ui_style_default(ui_style_t *style, const ui_font_t *font, int32_t view_width)
{
    // Sized for a font about 12 rows high on a 720-pixel screen: a title of
    // about 48 rows, text of about 36, a banner of about 24.
    const int32_t unit = (font != NULL && font->height > 0) ? font->height : 12;
    int32_t text = view_width / (20 * unit);
    if (text < 1) {
        text = 1;
    }
    style->font = font;
    style->text_scale = text;
    style->title_scale = text + (text + 2) / 3;
    style->banner_scale = text > 1 ? text - (text + 2) / 3 : 1;
    style->margin = view_width / 15;
    style->background = 0x000000;
    style->title = 0xFFFFFF;
    style->text = 0xC8C8C8;
    style->accent = 0xFFBE3C;
}

void ui_draw_screen(const ui_canvas_t *canvas, const ui_style_t *style, const p3a_ui_content_t *content)
{
    ui_canvas_clear(canvas, style->background);
    if (content == NULL || style->font == NULL) {
        return;
    }
    const int32_t vw = ui_canvas_view_width(canvas), vh = ui_canvas_view_height(canvas);
    const int32_t column = vw - 2 * style->margin;
    const int32_t gap = style->font->height * style->text_scale / 2;  // between the parts
    const int32_t line_gap = style->text_scale * 2;
    const int32_t bar_h = style->font->height * style->text_scale / 2;
    const bool has_title = content->title[0] != '\0', has_text = content->text[0] != '\0';
    const bool has_bar = content->progress >= 0;

    // Measure first, to centre the group.
    const int32_t title_h = has_title ? ui_text_block(NULL, style->font, 0, 0, column, style->title_scale, line_gap,
                                                      UI_ALIGN_CENTRE, 0, content->title, 2)
                                      : 0;
    const int32_t text_h = has_text ? ui_text_block(NULL, style->font, 0, 0, column, style->text_scale, line_gap,
                                                    UI_ALIGN_CENTRE, 0, content->text, MAX_LINES)
                                    : 0;
    int32_t total = title_h + text_h + (has_bar ? bar_h : 0);
    const int32_t parts = (has_title ? 1 : 0) + (has_text ? 1 : 0) + (has_bar ? 1 : 0);
    if (parts > 1) {
        total += (parts - 1) * gap;
    }
    int32_t y = (vh - total) / 2;
    if (y < style->margin / 2) {
        y = style->margin / 2;
    }
    if (has_title) {
        ui_text_block(canvas, style->font, style->margin, y, column, style->title_scale, line_gap, UI_ALIGN_CENTRE,
                      style->title, content->title, 2);
        y += title_h + gap;
    }
    if (has_text) {
        ui_text_block(canvas, style->font, style->margin, y, column, style->text_scale, line_gap, UI_ALIGN_CENTRE,
                      style->text, content->text, MAX_LINES);
        y += text_h + gap;
    }
    if (has_bar) {
        const int32_t edge = style->text_scale;  // the outline's thickness
        const int32_t progress = content->progress > 100 ? 100 : content->progress;
        ui_canvas_frame(canvas, style->margin, y, column, bar_h, edge, style->title);
        const int32_t inner = column - 4 * edge;
        ui_canvas_fill(canvas, style->margin + 2 * edge, y + 2 * edge, inner * progress / 100, bar_h - 4 * edge,
                       style->accent);
    }
}

int32_t ui_draw_banner(const ui_canvas_t *canvas, const ui_style_t *style, const p3a_ui_content_t *content)
{
    if (content == NULL || style->font == NULL) {
        return 0;
    }
    const int32_t vw = ui_canvas_view_width(canvas), vh = ui_canvas_view_height(canvas);
    const int32_t scale = style->banner_scale;
    const int32_t pad = style->font->height * scale / 2;
    const int32_t column = vw - 2 * pad;
    // A banner says one thing: the text if there is any, else the title.
    const char *words = content->text[0] != '\0' ? content->text : content->title;
    const int32_t words_h = ui_text_block(NULL, style->font, 0, 0, column, scale, scale, UI_ALIGN_CENTRE, 0, words, 2);
    if (words_h == 0) {
        return 0;
    }
    const int32_t height = words_h + 2 * pad;
    const int32_t top = vh - height;
    ui_canvas_fill(canvas, 0, top, vw, height, style->background);
    ui_canvas_fill(canvas, 0, top, vw, scale, style->accent);
    ui_text_block(canvas, style->font, pad, top + pad, column, scale, scale, UI_ALIGN_CENTRE, style->title, words, 2);
    return height;
}
