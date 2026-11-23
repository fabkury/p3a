// SPDX-License-Identifier: Apache-2.0
// The clock overlay: its text and where it is drawn.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_clock.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 720
#define H 720

static uint8_t *pixels;
static uint8_t font_bytes[64 * 1024];
static ui_font_t font;
static ui_canvas_t canvas;

static void fresh(int rotation)
{
    memset(pixels, 0x55, (size_t)W * H * 3);
    memset(pixels + (size_t)W * H * 3, 0xEE, 64);
    canvas = (ui_canvas_t){.pixels = pixels, .width = W, .height = H, .stride = W * 3, .rotation = rotation};
}

static uint32_t at(int x, int y)
{
    const uint8_t *p = pixels + ((size_t)y * W + (size_t)x) * 3;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

// The box around every pixel that was drawn on.
static void drawn_box(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = W;
    *y0 = H;
    *x1 = *y1 = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (at(x, y) != 0x555555) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
        }
    }
}

static void test_the_time_is_written_in_24_or_12_hours(void)
{
    char t[UI_CLOCK_TEXT_LEN];
    ui_clock_text(14, 5, true, t);
    TEST_ASSERT_EQUAL_STRING("14:05", t);
    ui_clock_text(0, 0, true, t);
    TEST_ASSERT_EQUAL_STRING("00:00", t);
    ui_clock_text(23, 59, true, t);
    TEST_ASSERT_EQUAL_STRING("23:59", t);
    ui_clock_text(14, 5, false, t);
    TEST_ASSERT_EQUAL_STRING("2:05", t);
    ui_clock_text(0, 30, false, t);
    TEST_ASSERT_EQUAL_STRING("12:30", t);  // half past midnight
    ui_clock_text(12, 0, false, t);
    TEST_ASSERT_EQUAL_STRING("12:00", t);
    ui_clock_text(99, -7, true, t);  // nonsense in: still a time, still terminated
    TEST_ASSERT_EQUAL(5, strlen(t));
}

static void test_each_of_the_nine_places_is_where_it_says(void)
{
    const ui_clock_style_t base = {.scale = 4, .colour = 0xFFFFFF, .outline_colour = 0x000000, .outline = 3};
    for (int place = 0; place < 9; place++) {
        ui_clock_style_t style = base;
        style.place = (ui_clock_place_t)place;
        fresh(0);
        ui_draw_clock(&canvas, &font, &style, "14:05");
        int x0, y0, x1, y1;
        drawn_box(&x0, &y0, &x1, &y1);
        TEST_ASSERT_TRUE(x1 > x0 && y1 > y0);
        const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        const int column = place % 3, row = place / 3;
        TEST_ASSERT_TRUE(column == 0 ? cx < W / 3 : column == 1 ? (cx > W / 3 && cx < 2 * W / 3) : cx > 2 * W / 3);
        TEST_ASSERT_TRUE(row == 0 ? cy < H / 3 : row == 1 ? (cy > H / 3 && cy < 2 * H / 3) : cy > 2 * H / 3);
        // Never against the edge, outline included.
        TEST_ASSERT_TRUE(x0 >= W / 30 && x1 < W - W / 30);
        TEST_ASSERT_TRUE(y0 >= H / 30 - 12 && y1 < H - H / 30);
        for (int i = 0; i < 64; i++) {
            TEST_ASSERT_EQUAL_HEX8(0xEE, pixels[(size_t)W * H * 3 + (size_t)i]);
        }
    }
}

static void test_the_outline_surrounds_every_stroke_and_follows_the_rotation(void)
{
    ui_clock_style_t style = {.place = UI_CLOCK_CENTRE, .scale = 6, .colour = 0xFFFFFF, .outline_colour = 0x102030, .outline = 4};
    fresh(0);
    ui_draw_clock(&canvas, &font, &style, "1");
    int white = 0, dark = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (at(x, y) == 0xFFFFFF) {
                white++;
                // Four pixels out in each direction it is still the outline or the digit, never the artwork.
                TEST_ASSERT_NOT_EQUAL(0x555555, at(x - 4, y));
                TEST_ASSERT_NOT_EQUAL(0x555555, at(x + 4, y));
                TEST_ASSERT_NOT_EQUAL(0x555555, at(x, y - 4));
                TEST_ASSERT_NOT_EQUAL(0x555555, at(x, y + 4));
            }
            dark += at(x, y) == 0x102030;
        }
    }
    TEST_ASSERT_TRUE(white > 0 && dark > white / 2);

    // With no outline only the digits are drawn.
    style.outline = 0;
    fresh(0);
    ui_draw_clock(&canvas, &font, &style, "1");
    int others = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            others += at(x, y) != 0xFFFFFF && at(x, y) != 0x555555;
        }
    }
    TEST_ASSERT_EQUAL(0, others);

    // Top left as the viewer sees it, with the device turned 90 degrees
    // clockwise, is the panel's top right.
    style.place = UI_CLOCK_TOP_LEFT;
    fresh(90);
    ui_draw_clock(&canvas, &font, &style, "14:05");
    int x0, y0, x1, y1;
    drawn_box(&x0, &y0, &x1, &y1);
    TEST_ASSERT_TRUE(x0 > W / 2 && y1 < H / 2);
}

int main(void)
{
    FILE *f = fopen(UI_FONT_FILE, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", UI_FONT_FILE);
        return 1;
    }
    const size_t len = fread(font_bytes, 1, sizeof(font_bytes), f);
    fclose(f);
    if (!ui_font_open(&font, font_bytes, len)) {
        return 1;
    }
    pixels = malloc((size_t)W * H * 3 + 64);
    UNITY_BEGIN();
    RUN_TEST(test_the_time_is_written_in_24_or_12_hours);
    RUN_TEST(test_each_of_the_nine_places_is_where_it_says);
    RUN_TEST(test_the_outline_surrounds_every_stroke_and_follows_the_rotation);
    const int failed = UNITY_END();
    free(pixels);
    return failed;
}
