// SPDX-License-Identifier: Apache-2.0
// The canvas (rotation, clipping) and text (font files, UTF-8, wrapping).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_canvas.h"
#include "ui_font.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 12
#define H 8

static uint8_t pixels[W * H * 3 + 16];
static ui_canvas_t canvas;

static void fresh(int rotation)
{
    memset(pixels, 0, sizeof(pixels));
    memset(pixels + W * H * 3, 0xEE, 16);  // a guard after the buffer
    canvas = (ui_canvas_t){.pixels = pixels, .width = W, .height = H, .stride = W * 3, .rotation = rotation};
}

static uint32_t at(int px, int py)  // a pixel of the panel, as 0xRRGGBB
{
    const uint8_t *p = pixels + (py * W + px) * 3;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static int lit(void)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        n += (pixels[i * 3] | pixels[i * 3 + 1] | pixels[i * 3 + 2]) != 0;
    }
    return n;
}

static void guard_intact(void)
{
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xEE, pixels[W * H * 3 + i]);
    }
}

// --- the canvas -----------------------------------------------------------------

static void test_a_fill_lands_where_the_viewer_sees_it_at_every_rotation(void)
{
    // The viewer's top left pixel, in red.
    fresh(0);
    ui_canvas_fill(&canvas, 0, 0, 1, 1, 0xFF0000);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, at(0, 0));
    TEST_ASSERT_EQUAL(1, lit());
    TEST_ASSERT_EQUAL(W, ui_canvas_view_width(&canvas));

    fresh(90);
    TEST_ASSERT_EQUAL(H, ui_canvas_view_width(&canvas));
    TEST_ASSERT_EQUAL(W, ui_canvas_view_height(&canvas));
    ui_canvas_fill(&canvas, 0, 0, 1, 1, 0xFF0000);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, at(W - 1, 0));

    fresh(180);
    ui_canvas_fill(&canvas, 0, 0, 1, 1, 0xFF0000);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, at(W - 1, H - 1));

    fresh(270);
    ui_canvas_fill(&canvas, 0, 0, 1, 1, 0xFF0000);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, at(0, H - 1));
    TEST_ASSERT_EQUAL(1, lit());

    // A 3 by 2 rectangle keeps its area, and its colour's byte order.
    for (int r = 0; r < 360; r += 90) {
        fresh(r);
        ui_canvas_fill(&canvas, 1, 2, 3, 2, 0x112233);
        TEST_ASSERT_EQUAL(6, lit());
    }
    fresh(0);
    ui_canvas_fill(&canvas, 1, 2, 3, 2, 0x112233);
    TEST_ASSERT_EQUAL_HEX8(0x33, pixels[(2 * W + 1) * 3]);      // blue first
    TEST_ASSERT_EQUAL_HEX8(0x11, pixels[(2 * W + 1) * 3 + 2]);  // red last
}

static void test_what_falls_outside_is_left_out_and_nothing_is_written_past_the_buffer(void)
{
    for (int r = 0; r < 360; r += 90) {
        fresh(r);
        ui_canvas_fill(&canvas, -5, -5, 7, 7, 0xFFFFFF);  // 2 by 2 remains
        TEST_ASSERT_EQUAL(4, lit());
        ui_canvas_fill(&canvas, 1000, 0, 5, 5, 0xFFFFFF);
        ui_canvas_fill(&canvas, 0, 0, 0, 5, 0xFFFFFF);
        ui_canvas_fill(&canvas, 0, 0, -3, 5, 0xFFFFFF);
        ui_canvas_fill(&canvas, INT32_MAX - 2, 0, 100, 100, 0xFFFFFF);
        TEST_ASSERT_EQUAL(4, lit());
        ui_canvas_fill(&canvas, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, 0xFFFFFF);
        ui_canvas_clear(&canvas, 0x010101);
        TEST_ASSERT_EQUAL(W * H, lit());
        guard_intact();
    }
}

static void test_a_frame_is_an_outline(void)
{
    fresh(0);
    ui_canvas_frame(&canvas, 1, 1, 6, 5, 1, 0xFFFFFF);
    TEST_ASSERT_EQUAL(2 * 6 + 2 * 3, lit());
    TEST_ASSERT_EQUAL_HEX32(0, at(3, 3));
    fresh(0);
    ui_canvas_frame(&canvas, 1, 1, 4, 4, 2, 0xFFFFFF);  // too thick to have a middle
    TEST_ASSERT_EQUAL(16, lit());
}

// --- fonts ----------------------------------------------------------------------

// A font of three characters, 3 rows high: ' ' (2 wide, empty), '?' (2 wide,
// a full block) and 'A' (3 wide: its first row and first column).
static size_t tiny_font(uint8_t *out)
{
    static const uint8_t file[] = {
        'P', '3', 'A', 'F', 1, 3, 2, 0, 3, 0,
        ' ', 0, 2, 0, 34, 0, 0, 0,
        '?', 0, 2, 0, 37, 0, 0, 0,
        'A', 0, 3, 0, 40, 0, 0, 0,
        0x00, 0x00, 0x00,
        0xC0, 0xC0, 0xC0,
        0xE0, 0x80, 0x80,
    };
    memcpy(out, file, sizeof(file));
    return sizeof(file);
}

static uint8_t font_bytes[64];
static ui_font_t font;

static void open_tiny(void)
{
    const size_t len = tiny_font(font_bytes);
    TEST_ASSERT_TRUE(ui_font_open(&font, font_bytes, len));
}

static void test_text_is_measured_and_drawn_at_whole_multiples(void)
{
    open_tiny();
    TEST_ASSERT_EQUAL(3, font.height);
    TEST_ASSERT_EQUAL(3, ui_text_width(&font, "A", 1));
    TEST_ASSERT_EQUAL(8, ui_text_width(&font, "A A", 1));
    TEST_ASSERT_EQUAL(24, ui_text_width(&font, "A A", 3));
    TEST_ASSERT_EQUAL(3, ui_text_width(&font, "A\nAAAA", 1));  // one line
    TEST_ASSERT_EQUAL(0, ui_text_width(&font, "", 2));
    TEST_ASSERT_EQUAL(0, ui_text_width(&font, "A", 0));

    fresh(0);
    TEST_ASSERT_EQUAL(3, ui_text_draw(&canvas, &font, 1, 1, 1, 0x00FF00, "A"));
    TEST_ASSERT_EQUAL(5, lit());  // three across the top, two more down the side
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, at(1, 1));
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, at(3, 1));
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, at(1, 3));
    TEST_ASSERT_EQUAL_HEX32(0, at(2, 2));

    fresh(0);
    ui_text_draw(&canvas, &font, 0, 0, 2, 0xFFFFFF, "A");
    TEST_ASSERT_EQUAL(20, lit());  // every pixel became 2 by 2

    // Off the edge: clipped, nothing written elsewhere.
    fresh(0);
    ui_text_draw(&canvas, &font, W - 2, H - 2, 4, 0xFFFFFF, "AAAA");
    TEST_ASSERT_EQUAL(4, lit());
    guard_intact();
}

static void test_utf8_is_read_and_what_the_font_lacks_becomes_a_question_mark(void)
{
    open_tiny();
    TEST_ASSERT_EQUAL(2, ui_text_width(&font, "B", 1));               // not in the font
    TEST_ASSERT_EQUAL(2, ui_text_width(&font, "\xC3\xA9", 1));        // e acute: one character
    TEST_ASSERT_EQUAL(2, ui_text_width(&font, "\xE2\x82\xAC", 1));    // the euro sign: one
    TEST_ASSERT_EQUAL(2, ui_text_width(&font, "\xF0\x9F\x99\x82", 1)); // an emoji: one
    // Malformed: a lone continuation byte, a sequence cut short. Each bad
    // byte is one '?', and the string still ends.
    TEST_ASSERT_EQUAL(2, ui_text_width(&font, "\x80", 1));
    TEST_ASSERT_EQUAL(4, ui_text_width(&font, "\xE2\x82", 1));
    TEST_ASSERT_EQUAL(5, ui_text_width(&font, "\xC3" "A", 1));
}

static void test_a_block_wraps_at_spaces_breaks_long_words_and_aligns(void)
{
    open_tiny();
    // "A A A": 3+2+3+2+3 = 13 wide. In a column of 9: "A A" then "A".
    TEST_ASSERT_EQUAL(3 + 1 + 3, ui_text_block(NULL, &font, 0, 0, 9, 1, 1, UI_ALIGN_LEFT, 0, "A A A", 10));
    TEST_ASSERT_EQUAL(3, ui_text_block(NULL, &font, 0, 0, 100, 1, 1, UI_ALIGN_LEFT, 0, "A A A", 10));
    // A newline always breaks; an empty line has height.
    TEST_ASSERT_EQUAL(3 * 3 + 2, ui_text_block(NULL, &font, 0, 0, 100, 1, 1, UI_ALIGN_LEFT, 0, "A\n\nA", 10));
    // A word wider than the column is broken where it must be: "AAAAA" in 7 is "AA", "AA", "A".
    TEST_ASSERT_EQUAL(9, ui_text_block(NULL, &font, 0, 0, 7, 1, 0, UI_ALIGN_LEFT, 0, "AAAAA", 10));
    // No more than the lines allowed.
    TEST_ASSERT_EQUAL(6, ui_text_block(NULL, &font, 0, 0, 7, 1, 0, UI_ALIGN_LEFT, 0, "AAAAA", 2));
    // A column narrower than any character still ends.
    TEST_ASSERT_TRUE(ui_text_block(NULL, &font, 0, 0, 1, 1, 0, UI_ALIGN_LEFT, 0, "AAA", 10) > 0);
    TEST_ASSERT_EQUAL(0, ui_text_block(NULL, &font, 0, 0, 0, 1, 0, UI_ALIGN_LEFT, 0, "AAA", 10));
    TEST_ASSERT_EQUAL(0, ui_text_block(NULL, &font, 0, 0, 9, 1, 0, UI_ALIGN_LEFT, 0, "", 10));

    // Alignment: one 'A' (3 wide) in a column of 9 starting at x = 1.
    fresh(0);
    ui_text_block(&canvas, &font, 1, 0, 9, 1, 0, UI_ALIGN_LEFT, 0xFFFFFF, "A", 1);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, at(1, 0));
    fresh(0);
    ui_text_block(&canvas, &font, 1, 0, 9, 1, 0, UI_ALIGN_CENTRE, 0xFFFFFF, "A", 1);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, at(4, 0));
    TEST_ASSERT_EQUAL_HEX32(0, at(1, 0));
    fresh(0);
    ui_text_block(&canvas, &font, 1, 0, 9, 1, 0, UI_ALIGN_RIGHT, 0xFFFFFF, "A", 1);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, at(7, 0));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, at(9, 0));
}

static void test_a_file_that_is_not_a_font_is_refused(void)
{
    uint8_t bytes[64];
    const size_t len = tiny_font(bytes);
    ui_font_t f;
    TEST_ASSERT_FALSE(ui_font_open(&f, NULL, len));
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, 9));
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len - 1));  // the last bitmap is cut short
    bytes[0] = 'X';
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    tiny_font(bytes);
    bytes[4] = 2;  // a version this code does not know
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    tiny_font(bytes);
    bytes[5] = 0;  // no height
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    tiny_font(bytes);
    bytes[8] = 200;  // more characters than the file has room for
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    tiny_font(bytes);
    bytes[10 + 8] = ' ';  // out of order
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    tiny_font(bytes);
    bytes[10 + 4] = 5;  // a bitmap inside the table
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    tiny_font(bytes);
    bytes[10 + 16 + 2] = 255;  // a width whose bitmap runs past the end
    TEST_ASSERT_FALSE(ui_font_open(&f, bytes, len));
    // A font that failed to open draws nothing and measures nothing.
    TEST_ASSERT_EQUAL(0, ui_text_width(&f, "A", 1));
    fresh(0);
    ui_text_draw(&canvas, &f, 0, 0, 1, 0xFFFFFF, "A");
    TEST_ASSERT_EQUAL(0, lit());
}

// Any bytes at all: a font either fails to open or draws without reading
// outside its file (the sanitizers watch) or writing outside the canvas.
static void test_damaged_font_files_are_refused_or_harmless(void)
{
    uint8_t good[64];
    const size_t len = tiny_font(good);
    uint32_t seed = 12345;
    for (int round = 0; round < 20000; round++) {
        uint8_t *bytes = malloc(len);  // exactly the file: an overrun is seen
        memcpy(bytes, good, len);
        const int flips = 1 + round % 3;
        for (int i = 0; i < flips; i++) {
            seed = seed * 1664525u + 1013904223u;
            bytes[(seed >> 8) % len] = (uint8_t)(seed >> 24);
        }
        seed = seed * 1664525u + 1013904223u;
        const size_t cut = round % 4 == 0 ? (seed >> 8) % (len + 1) : len;
        ui_font_t f;
        if (ui_font_open(&f, bytes, cut)) {
            fresh(round % 4 * 90);
            ui_text_draw(&canvas, &f, 0, 0, 1, 0xFFFFFF, "A ?B\xC3\xA9");
            ui_text_block(&canvas, &f, 0, 0, 7, 1, 0, UI_ALIGN_CENTRE, 0xFFFFFF, "AA A? A\nA", 5);
            guard_intact();
        }
        free(bytes);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_fill_lands_where_the_viewer_sees_it_at_every_rotation);
    RUN_TEST(test_what_falls_outside_is_left_out_and_nothing_is_written_past_the_buffer);
    RUN_TEST(test_a_frame_is_an_outline);
    RUN_TEST(test_text_is_measured_and_drawn_at_whole_multiples);
    RUN_TEST(test_utf8_is_read_and_what_the_font_lacks_becomes_a_question_mark);
    RUN_TEST(test_a_block_wraps_at_spaces_breaks_long_words_and_aligns);
    RUN_TEST(test_a_file_that_is_not_a_font_is_refused);
    RUN_TEST(test_damaged_font_files_are_refused_or_harmless);
    return UNITY_END();
}
