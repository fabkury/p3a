// SPDX-License-Identifier: Apache-2.0
// Draws a piece of the UI into a file on the workstation, with the code the
// device runs.
//
//   ui_preview <font> <rotation> screen    <out.bgr> <progress> <title> <text> [hint]
//   ui_preview <font> <rotation> banner    <out.bgr> <progress> <title> <text> [under.bgr]
//   ui_preview <font> <rotation> clock     <out.bgr> <place 0-8> <scale> <colour> <outline colour> <outline> <text> [under.bgr]
//   ui_preview <font> <rotation> icon      <out.bgr> <left 0|1> <file.p3aimg> [under.bgr]
//   ui_preview <font> <rotation> indicator <out.bgr> <size> <colour> [under.bgr]
//
// The output is the 720 x 720 framebuffer as the panel has it: three bytes
// a pixel (blue, green, red), top row first, no header. The overlays are
// drawn over `under.bgr` (the same format) when given, else over black.
// `progress` is 0 to 100, or -1 for none; colours are RRGGBB in hex.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_clock.h"
#include "ui_draw.h"

#define W 720
#define H 720

static uint8_t pixels[W * H * 3];
static uint8_t font_bytes[256 * 1024];
static uint8_t image_bytes[1024 * 1024 + 16];

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

static void under(int argc, char **argv, int at)
{
    if (argc > at && read_file(argv[at], pixels, sizeof(pixels)) != sizeof(pixels)) {
        fprintf(stderr, "%s is not a %d x %d frame\n", argv[at], W, H);
        exit(1);
    }
}

static ui_colour_t colour(const char *hex)
{
    return (ui_colour_t)strtoul(hex, NULL, 16);
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: ui_preview <font> <rotation> screen|banner|clock|icon|indicator <out.bgr> ...; see the file's head\n");
        return 2;
    }
    ui_font_t font;
    if (!ui_font_open(&font, font_bytes, read_file(argv[1], font_bytes, sizeof(font_bytes)))) {
        fprintf(stderr, "%s is not a font\n", argv[1]);
        return 1;
    }
    const ui_canvas_t canvas = {.pixels = pixels, .width = W, .height = H, .stride = W * 3, .rotation = atoi(argv[2])};
    const char *mode = argv[3];
    ui_style_t style;
    ui_style_default(&style, &font, ui_canvas_view_width(&canvas));

    if ((strcmp(mode, "screen") == 0 || strcmp(mode, "banner") == 0) && argc >= 8) {
        p3a_ui_content_t content;
        p3a_ui_content(&content, argv[6], atoi(argv[5]), "%s", argv[7]);
        if (strcmp(mode, "banner") == 0) {
            under(argc, argv, 8);
            ui_draw_banner(&canvas, &style, &content);
        } else {
            if (argc > 8) {
                snprintf(content.hint, sizeof(content.hint), "%s", argv[8]);
            }
            ui_draw_screen(&canvas, &style, &content);
        }
    } else if (strcmp(mode, "clock") == 0 && argc >= 11) {
        under(argc, argv, 11);
        const ui_clock_style_t clock = {
            .place = (ui_clock_place_t)atoi(argv[5]),
            .scale = atoi(argv[6]),
            .colour = colour(argv[7]),
            .outline_colour = colour(argv[8]),
            .outline = atoi(argv[9]),
        };
        ui_draw_clock(&canvas, &font, &clock, argv[10]);
    } else if (strcmp(mode, "icon") == 0 && argc >= 7) {
        under(argc, argv, 7);
        ui_image_t image;
        if (!ui_image_open(&image, image_bytes, read_file(argv[6], image_bytes, sizeof(image_bytes)))) {
            fprintf(stderr, "%s is not a picture\n", argv[6]);
            return 1;
        }
        ui_draw_icon(&canvas, &image, atoi(argv[5]) != 0);
    } else if (strcmp(mode, "indicator") == 0 && argc >= 7) {
        under(argc, argv, 7);
        ui_draw_indicator(&canvas, atoi(argv[5]), colour(argv[6]));
    } else {
        fprintf(stderr, "unknown mode or too few arguments: %s\n", mode);
        return 2;
    }

    FILE *out = fopen(argv[4], "wb");
    if (out == NULL || fwrite(pixels, 1, sizeof(pixels), out) != sizeof(pixels)) {
        fprintf(stderr, "cannot write %s\n", argv[4]);
        return 1;
    }
    fclose(out);
    return 0;
}
