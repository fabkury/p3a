// SPDX-License-Identifier: Apache-2.0
// Draws one of the UI's screens into a file on the workstation, with the
// code the device runs.
//
//   ui_preview <font.p3afont> <rotation> screen <out.bgr> <progress> <title> <text> [hint]
//   ui_preview <font.p3afont> <rotation> banner <out.bgr> <progress> <title> <text> [under.bgr]
//
// The output is the 720 x 720 framebuffer as the panel has it: three bytes
// a pixel (blue, green, red), top row first, no header. A banner is drawn
// over `under.bgr` (the same format) when given, else over black.
// `progress` is 0 to 100, or -1 for none.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_draw.h"

#define W 720
#define H 720

static uint8_t pixels[W * H * 3];
static uint8_t font_bytes[256 * 1024];

static size_t read_file(const char *path, uint8_t *into, size_t room)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    const size_t n = fread(into, 1, room, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: ui_preview <font> <rotation> screen|banner <out.bgr> <progress> <title> <text> [hint | under.bgr]\n");
        return 2;
    }
    ui_font_t font;
    if (!ui_font_open(&font, font_bytes, read_file(argv[1], font_bytes, sizeof(font_bytes)))) {
        fprintf(stderr, "%s is not a font\n", argv[1]);
        return 1;
    }
    const ui_canvas_t canvas = {.pixels = pixels, .width = W, .height = H, .stride = W * 3, .rotation = atoi(argv[2])};
    ui_style_t style;
    ui_style_default(&style, &font, ui_canvas_view_width(&canvas));
    p3a_ui_content_t content;
    p3a_ui_content(&content, argv[6], atoi(argv[5]), "%s", argv[7]);
    if (strcmp(argv[3], "banner") == 0) {
        if (argc > 8 && read_file(argv[8], pixels, sizeof(pixels)) != sizeof(pixels)) {
            fprintf(stderr, "%s is not a %d x %d frame\n", argv[8], W, H);
            return 1;
        }
        ui_draw_banner(&canvas, &style, &content);
    } else {
        if (argc > 8) {
            snprintf(content.hint, sizeof(content.hint), "%s", argv[8]);
        }
        ui_draw_screen(&canvas, &style, &content);
    }
    FILE *out = fopen(argv[4], "wb");
    if (out == NULL || fwrite(pixels, 1, sizeof(pixels), out) != sizeof(pixels)) {
        fprintf(stderr, "cannot write %s\n", argv[4]);
        return 1;
    }
    fclose(out);
    return 0;
}
