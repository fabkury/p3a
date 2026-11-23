// SPDX-License-Identifier: Apache-2.0
// Small pictures of the UI's own. See ui_image.h. Pure.

#include "ui_image.h"

#include <string.h>

#define HEADER_LEN 10

bool ui_image_open(ui_image_t *image, const uint8_t *data, size_t len)
{
    memset(image, 0, sizeof(*image));
    if (data == NULL || len < HEADER_LEN || memcmp(data, "P3AI", 4) != 0 || data[4] != 1) {
        return false;
    }
    const int32_t width = data[6] | (data[7] << 8), height = data[8] | (data[9] << 8);
    if (width == 0 || height == 0 || width > UI_IMAGE_MAX_SIDE || height > UI_IMAGE_MAX_SIDE ||
        (size_t)width * (size_t)height * 4 != len - HEADER_LEN) {
        return false;
    }
    image->pixels = data + HEADER_LEN;
    image->width = width;
    image->height = height;
    return true;
}

void ui_image_draw(const ui_canvas_t *canvas, const ui_image_t *image, int32_t x, int32_t y, int32_t scale)
{
    if (image->pixels == NULL || scale <= 0) {
        return;
    }
    for (int32_t row = 0; row < image->height; row++) {
        const uint8_t *p = image->pixels + (size_t)row * (size_t)image->width * 4;
        int32_t col = 0;
        while (col < image->width) {
            if (p[col * 4 + 3] < 128) {
                col++;
                continue;
            }
            // A run of one colour is one rectangle: icons are mostly flat.
            const int32_t from = col;
            const uint8_t *first = p + col * 4;
            while (col < image->width && p[col * 4 + 3] >= 128 && memcmp(p + col * 4, first, 3) == 0) {
                col++;
            }
            const ui_colour_t colour = (ui_colour_t)first[0] | ((ui_colour_t)first[1] << 8) | ((ui_colour_t)first[2] << 16);
            ui_canvas_fill(canvas, x + from * scale, y + row * scale, (col - from) * scale, scale, colour);
        }
    }
}
