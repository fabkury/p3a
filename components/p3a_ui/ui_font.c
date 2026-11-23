// SPDX-License-Identifier: Apache-2.0
// Text. See ui_font.h. Pure.

#include "ui_font.h"

#include <string.h>

#define HEADER_LEN 10
#define ENTRY_LEN  8

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool ui_font_open(ui_font_t *font, const uint8_t *data, size_t len)
{
    memset(font, 0, sizeof(*font));
    if (data == NULL || len < HEADER_LEN || memcmp(data, "P3AF", 4) != 0 || data[4] != 1) {
        return false;
    }
    const uint8_t height = data[5], baseline = data[6];
    const uint16_t count = u16(data + 8);
    if (height == 0 || baseline > height || count == 0 || (size_t)count * ENTRY_LEN > len - HEADER_LEN) {
        return false;
    }
    const size_t table_end = HEADER_LEN + (size_t)count * ENTRY_LEN;
    uint32_t previous = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = data + HEADER_LEN + (size_t)i * ENTRY_LEN;
        const uint16_t codepoint = u16(e);
        const size_t bytes = (size_t)((e[2] + 7) / 8) * height;
        const uint32_t offset = u32(e + 4);
        if ((i > 0 && codepoint <= previous) || offset < table_end || offset > len || bytes > len - offset) {
            return false;
        }
        previous = codepoint;
    }
    font->data = data;
    font->len = len;
    font->height = height;
    font->baseline = baseline;
    font->count = count;
    return true;
}

// The table entry of a character, or NULL.
static const uint8_t *find(const ui_font_t *font, uint32_t codepoint)
{
    int32_t lo = 0, hi = (int32_t)font->count - 1;
    while (lo <= hi) {
        const int32_t mid = (lo + hi) / 2;
        const uint8_t *e = font->data + HEADER_LEN + (size_t)mid * ENTRY_LEN;
        const uint16_t at = u16(e);
        if (at == codepoint) {
            return e;
        }
        if (at < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static const uint8_t *glyph(const ui_font_t *font, uint32_t codepoint)
{
    const uint8_t *e = find(font, codepoint);
    return e != NULL ? e : find(font, '?');
}

// Reads one character of UTF-8 and advances. Malformed bytes come out as
// U+FFFD, one byte at a time, so that a bad string still ends.
static uint32_t next_char(const char **text)
{
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t c = s[0];
    int extra = 0;
    if (c < 0x80) {
        extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1;
        c &= 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        c &= 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        c &= 0x07;
    } else {
        *text += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {  // also stops at the terminator
            *text += 1;
            return 0xFFFD;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *text += extra + 1;
    return c;
}

// Measures, and draws when there is a canvas, the text from `text` up to
// `end` (or a newline or the terminator).
static int32_t run(const ui_canvas_t *canvas, const ui_font_t *font, int32_t x, int32_t y, int32_t scale,
                   ui_colour_t colour, const char *text, const char *end)
{
    if (font->data == NULL || scale <= 0) {
        return 0;
    }
    int32_t pen = 0;
    while ((end == NULL || text < end) && *text != '\0' && *text != '\n') {
        const uint8_t *e = glyph(font, next_char(&text));
        if (e == NULL) {
            continue;
        }
        const int32_t advance = e[2];
        if (canvas != NULL) {
            const uint8_t *rows = font->data + u32(e + 4);
            const int32_t row_bytes = (advance + 7) / 8;
            for (int32_t r = 0; r < font->height; r++) {
                const uint8_t *row = rows + (size_t)r * (size_t)row_bytes;
                // Ink in runs: one rectangle per stretch of set bits.
                int32_t c = 0;
                while (c < advance) {
                    if (!(row[c / 8] & (0x80 >> (c % 8)))) {
                        c++;
                        continue;
                    }
                    const int32_t from = c;
                    while (c < advance && (row[c / 8] & (0x80 >> (c % 8)))) {
                        c++;
                    }
                    ui_canvas_fill(canvas, x + (pen + from) * scale, y + r * scale, (c - from) * scale, scale, colour);
                }
            }
        }
        pen += advance;
    }
    return pen * scale;
}

int32_t ui_text_width(const ui_font_t *font, const char *text, int32_t scale)
{
    return text ? run(NULL, font, 0, 0, scale, 0, text, NULL) : 0;
}

int32_t ui_text_draw(const ui_canvas_t *canvas, const ui_font_t *font, int32_t x, int32_t y, int32_t scale,
                     ui_colour_t colour, const char *text)
{
    return text ? run(canvas, font, x, y, scale, colour, text, NULL) : 0;
}

int32_t ui_text_block(const ui_canvas_t *canvas, const ui_font_t *font, int32_t x, int32_t y, int32_t width,
                      int32_t scale, int32_t line_gap, ui_align_t align, ui_colour_t colour, const char *text,
                      int32_t max_lines)
{
    if (text == NULL || font->data == NULL || scale <= 0 || width <= 0) {
        return 0;
    }
    const int32_t line_height = font->height * scale;
    int32_t lines = 0;
    const char *p = text;
    while (*p != '\0' && lines < max_lines) {
        // Find where this line ends: the last space that still fits, or
        // where a word too wide for the column must be broken.
        const char *scan = p, *fit_end = p, *last_space = NULL;
        while (*scan != '\0' && *scan != '\n') {
            const char *before = scan;
            const uint32_t c = next_char(&scan);
            if (run(NULL, font, 0, 0, scale, 0, p, scan) > width && before != p) {
                scan = before;
                break;
            }
            if (c == ' ') {
                last_space = before;
            }
            fit_end = scan;
        }
        const char *line_end = fit_end, *next = fit_end;
        if (*scan != '\0' && *scan != '\n') {
            // The line is full part way through the text.
            if (*scan == ' ') {
                next = scan + 1;
            } else if (last_space != NULL) {
                line_end = last_space;
                next = last_space + 1;
            }
        } else if (*scan == '\n') {
            next = scan + 1;
        }
        if (canvas != NULL) {
            const int32_t w = run(NULL, font, 0, 0, scale, 0, p, line_end);
            const int32_t left = align == UI_ALIGN_CENTRE ? x + (width - w) / 2 : align == UI_ALIGN_RIGHT ? x + width - w : x;
            run(canvas, font, left, y + lines * (line_height + line_gap), scale, colour, p, line_end);
        }
        lines++;
        if (next == p) {
            break;  // nothing fits at all: a column narrower than one character
        }
        p = next;
    }
    return lines == 0 ? 0 : lines * line_height + (lines - 1) * line_gap;
}
