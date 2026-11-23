// SPDX-License-Identifier: Apache-2.0
// What the screens look like. See ui_draw.h. Pure.

#include "ui_draw.h"

#include <string.h>

#define MAX_LINES 8

// Draws (or, with no canvas, measures) a screen's text: each line a centred
// paragraph, or a row of label and value when it has a tab. Returns the
// height used.
static int32_t body(const ui_canvas_t *canvas, const ui_style_t *style, int32_t x, int32_t y, int32_t column,
                    const char *text)
{
    const int32_t scale = style->text_scale, line_gap = scale * 2;
    const int32_t label_w = column * 2 / 5, value_x = x + label_w + scale * 4, value_w = column - label_w - scale * 4;
    int32_t used = 0;
    const char *line = text;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        char piece[P3A_UI_TEXT_LEN];
        const size_t take = len < sizeof(piece) - 1 ? len : sizeof(piece) - 1;
        memcpy(piece, line, take);
        piece[take] = '\0';
        if (used > 0) {
            used += line_gap;
        }
        char *tab = strchr(piece, '\t');
        if (tab != NULL) {
            *tab = '\0';
            const int32_t a = ui_text_block(canvas, style->font, x, y + used, label_w, scale, line_gap, UI_ALIGN_LEFT,
                                            style->label, piece, 1);
            const int32_t b = ui_text_block(canvas, style->font, value_x, y + used, value_w, scale, line_gap,
                                            UI_ALIGN_LEFT, style->title, tab + 1, 2);
            used += a > b ? a : b;
        } else if (piece[0] == '\0') {
            used += style->font->height * scale / 2;  // an empty line is half a line of space
        } else {
            used += ui_text_block(canvas, style->font, x, y + used, column, scale, line_gap, UI_ALIGN_CENTRE,
                                  style->text, piece, MAX_LINES);
        }
        line = end ? end + 1 : line + len;
    }
    return used;
}

// Where things are at the bottom of a screen, from the bottom edge up: the
// hint, then the buttons. The same arithmetic places them and finds them
// again under a finger.
static int32_t hint_height(const ui_style_t *style, const p3a_ui_content_t *content)
{
    return content->hint[0] != '\0' ? style->font->height * style->banner_scale + style->margin / 2 : 0;
}

static int button_count(const p3a_ui_content_t *content)
{
    int n = 0;
    while (n < P3A_UI_BUTTONS && content->buttons[n][0] != '\0') {
        n++;
    }
    return n;
}

bool ui_draw_button_rect(const ui_style_t *style, int32_t view_width, int32_t view_height,
                         const p3a_ui_content_t *content, int index, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    const int count = content != NULL && style->font != NULL ? button_count(content) : 0;
    if (index < 0 || index >= count) {
        return false;
    }
    const int32_t height = style->font->height * style->text_scale * 5 / 2;  // 90 pixels on the panel: a finger's worth
    const int32_t gap = style->margin / 2;
    const int32_t bottom = view_height - style->margin / 2 - hint_height(style, content);
    *w = (view_width - 2 * style->margin) * 3 / 4;
    *h = height;
    *x = (view_width - *w) / 2;
    *y = bottom - (count - index) * height - (count - 1 - index) * gap;
    return true;
}

int ui_draw_button_at(const ui_style_t *style, int32_t view_width, int32_t view_height, const p3a_ui_content_t *content,
                      int32_t tap_x, int32_t tap_y)
{
    for (int i = 0; i < P3A_UI_BUTTONS; i++) {
        int32_t x, y, w, h;
        if (ui_draw_button_rect(style, view_width, view_height, content, i, &x, &y, &w, &h) && tap_x >= x &&
            tap_x < x + w && tap_y >= y && tap_y < y + h) {
            return (content->buttons_enabled >> i) & 1 ? i : -1;  // a dimmed button takes the tap and does nothing
        }
    }
    return -1;
}

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
    style->label = 0x8C8C8C;
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
    const int32_t text_h = has_text ? body(NULL, style, style->margin, 0, column, content->text) : 0;
    int32_t total = title_h + text_h + (has_bar ? bar_h : 0);
    const int32_t parts = (has_title ? 1 : 0) + (has_text ? 1 : 0) + (has_bar ? 1 : 0);
    if (parts > 1) {
        total += (parts - 1) * gap;
    }
    // The group is centred in what the buttons and the hint leave free.
    int32_t free_h = vh;
    int32_t bx, by, bw, bh;
    if (ui_draw_button_rect(style, vw, vh, content, 0, &bx, &by, &bw, &bh)) {
        free_h = by - style->margin / 2;
    } else {
        free_h = vh - hint_height(style, content);
    }
    int32_t y = (free_h - total) / 2;
    if (y < style->margin / 2) {
        y = style->margin / 2;
    }
    if (has_title) {
        ui_text_block(canvas, style->font, style->margin, y, column, style->title_scale, line_gap, UI_ALIGN_CENTRE,
                      style->title, content->title, 2);
        y += title_h + gap;
    }
    if (has_text) {
        body(canvas, style, style->margin, y, column, content->text);
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
    for (int i = 0; ui_draw_button_rect(style, vw, vh, content, i, &bx, &by, &bw, &bh); i++) {
        const bool enabled = (content->buttons_enabled >> i) & 1;
        const ui_colour_t colour = enabled ? style->title : style->label;
        ui_canvas_fill(canvas, bx, by, bw, bh, style->background);
        ui_canvas_frame(canvas, bx, by, bw, bh, style->text_scale, colour);
        const int32_t label_h = style->font->height * style->text_scale;
        ui_text_block(canvas, style->font, bx, by + (bh - label_h) / 2, bw, style->text_scale, 0, UI_ALIGN_CENTRE, colour,
                      content->buttons[i], 1);
    }
    if (content->hint[0] != '\0') {
        // What a person can do here, small, at the bottom edge.
        const int32_t hint_h = style->font->height * style->banner_scale;
        ui_text_block(canvas, style->font, style->margin, vh - style->margin / 2 - hint_h, column, style->banner_scale, 0,
                      UI_ALIGN_CENTRE, style->label, content->hint, 1);
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
