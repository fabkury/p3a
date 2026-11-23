// SPDX-License-Identifier: Apache-2.0
// The screens' layout, drawn with the built-in font: everything stays on
// the screen and in its place, whatever the words and the rotation.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_draw.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 720
#define H 720

static uint8_t *pixels;  // W * H * 3 and a guard
static uint8_t font_bytes[64 * 1024];
static ui_font_t font;
static ui_canvas_t canvas;
static ui_style_t style;

static void fresh(int rotation)
{
    memset(pixels, 0x55, (size_t)W * H * 3);  // not a colour the UI uses
    memset(pixels + (size_t)W * H * 3, 0xEE, 64);
    canvas = (ui_canvas_t){.pixels = pixels, .width = W, .height = H, .stride = W * 3, .rotation = rotation};
    ui_style_default(&style, &font, ui_canvas_view_width(&canvas));
}

static uint32_t at(int x, int y)
{
    const uint8_t *p = pixels + ((size_t)y * W + (size_t)x) * 3;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

// The rows that have any pixel of `colour`: first and last, or -1.
static void rows_with(uint32_t colour, int *first, int *last)
{
    *first = *last = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (at(x, y) == colour) {
                if (*first < 0) {
                    *first = y;
                }
                *last = y;
                break;
            }
        }
    }
}

static int count(uint32_t colour)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            n += at(x, y) == colour;
        }
    }
    return n;
}

static void guard_intact(void)
{
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xEE, pixels[(size_t)W * H * 3 + (size_t)i]);
    }
}

static void test_the_built_in_font_opens_and_the_default_style_suits_the_panel(void)
{
    fresh(0);
    TEST_ASSERT_TRUE(font.height >= 6 && font.height <= 32);
    TEST_ASSERT_TRUE(style.text_scale >= 1);
    TEST_ASSERT_TRUE(style.title_scale > style.text_scale);
    TEST_ASSERT_TRUE(style.banner_scale <= style.text_scale);
    // A line of text is between a twentieth and a twelfth of the screen.
    const int line = font.height * style.text_scale;
    TEST_ASSERT_TRUE(line >= H / 24 && line <= H / 12);
    // Every printable ASCII character is in the font (none is as wide as '?' by accident only).
    TEST_ASSERT_TRUE(ui_text_width(&font, "p3a-fab.local 192.168.4.42", 1) > 26 * 2);
}

static void test_a_screen_is_title_then_text_then_bar_centred_on_the_background(void)
{
    p3a_ui_content_t c;
    p3a_ui_content(&c, "Updating Wi-Fi", 64, "2.9.1 to 2.9.3. Do not unplug");
    fresh(0);
    ui_draw_screen(&canvas, &style, &c);
    TEST_ASSERT_EQUAL(0, count(0x555555));  // the whole screen was painted
    int t0, t1, x0, x1, b0, b1;
    rows_with(style.title, &t0, &t1);  // the title, and the bar's outline below
    rows_with(style.text, &x0, &x1);
    rows_with(style.accent, &b0, &b1);
    TEST_ASSERT_TRUE(t0 > 0 && x0 > t0 && b0 > x1);  // in that order, top to bottom
    // Centred as a group, by its line boxes: the ink may sit a few rows off.
    TEST_ASSERT_INT_WITHIN(24u, t0, H - 1 - t1);
    // The bar is filled to 64% of its inside, give or take a pixel's rounding.
    int fill = 0, room = 0;
    for (int x = 0; x < W; x++) {
        fill += at(x, (b0 + b1) / 2) == style.accent;
        room += at(x, (b0 + b1) / 2) != style.title;
    }
    room -= 2 * style.margin;  // the screen outside the bar
    TEST_ASSERT_INT_WITHIN((unsigned)(room / 50 + 8), room * 64 / 100, fill);
    // Left and right margins are kept.
    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_HEX32(style.background, at(style.margin - 1, y));
        TEST_ASSERT_EQUAL_HEX32(style.background, at(W - style.margin, y));
    }
    guard_intact();
}

static void test_no_words_can_push_anything_off_the_screen(void)
{
    p3a_ui_content_t c;
    memset(&c, 'W', sizeof(c));  // the widest letters, no spaces, to the last byte
    c.title[sizeof(c.title) - 1] = '\0';
    c.text[sizeof(c.text) - 1] = '\0';
    c.progress = 250;  // beyond the end
    for (int r = 0; r < 360; r += 90) {
        fresh(r);
        ui_draw_screen(&canvas, &style, &c);
        guard_intact();
        TEST_ASSERT_EQUAL(0, count(0x555555));
        fresh(r);
        ui_draw_banner(&canvas, &style, &c);
        guard_intact();
    }
    // Nothing to say: a plain background, and no banner at all.
    p3a_ui_content(&c, "", -1, "%s", "");
    fresh(0);
    ui_draw_screen(&canvas, &style, &c);
    TEST_ASSERT_EQUAL(W * H, count(style.background));
    fresh(0);
    TEST_ASSERT_EQUAL(0, ui_draw_banner(&canvas, &style, &c));
    TEST_ASSERT_EQUAL(W * H, count(0x555555));
    fresh(0);
    ui_draw_screen(&canvas, &style, NULL);
    TEST_ASSERT_EQUAL(W * H, count(style.background));
}

static void test_a_banner_is_a_band_at_the_viewers_bottom_and_leaves_the_rest(void)
{
    p3a_ui_content_t c;
    p3a_ui_content(&c, "Not shown", -1, "That artwork is no longer on the device");
    fresh(0);
    const int height = ui_draw_banner(&canvas, &style, &c);
    TEST_ASSERT_TRUE(height > 0 && height < H / 5);
    TEST_ASSERT_EQUAL((W * (H - height)), count(0x555555));  // everything above is untouched
    TEST_ASSERT_EQUAL_HEX32(style.accent, at(W / 2, H - height));  // its top edge
    TEST_ASSERT_TRUE(count(style.title) > 100);  // its words

    // Turned 90 degrees, the viewer's bottom is the panel's left edge.
    fresh(90);
    TEST_ASSERT_EQUAL(height, ui_draw_banner(&canvas, &style, &c));
    TEST_ASSERT_EQUAL_HEX32(0x555555, at(height, H / 2));
    TEST_ASSERT_NOT_EQUAL(0x555555, at(height - 1, H / 2));
    TEST_ASSERT_EQUAL_HEX32(style.accent, at(height - 1, H / 2));

    // With no text the title is what it says.
    p3a_ui_content(&c, "Card repaired", -1, "%s", "");
    fresh(0);
    TEST_ASSERT_TRUE(ui_draw_banner(&canvas, &style, &c) > 0);
}

static void test_rows_line_up_in_two_columns_and_the_hint_sits_at_the_bottom(void)
{
    p3a_ui_content_t c;
    p3a_ui_content(&c, "p3a-fab", -1, "Firmware\t2.0.0\nAddress\t192.168.4.42\nSD card\t29.1 of 29.7 GB free");
    snprintf(c.hint, sizeof(c.hint), "Long press to close");
    fresh(0);
    ui_draw_screen(&canvas, &style, &c);
    guard_intact();
    // Labels are in the label colour and all start at the left margin;
    // values are in the title's colour and all start at the same column.
    const int column = W - 2 * style.margin;
    const int value_x = style.margin + column * 2 / 5 + style.text_scale * 4;
    int label_left = W, value_left = W, label_rows = 0;
    for (int y = 0; y < H - H / 8; y++) {  // above the hint
        bool any_label = false;
        for (int x = 0; x < W; x++) {
            if (at(x, y) == style.label) {
                any_label = true;
                if (x < label_left) {
                    label_left = x;
                }
                TEST_ASSERT_TRUE(x < value_x);
            }
        }
        label_rows += any_label;
    }
    TEST_ASSERT_INT_WITHIN((unsigned)(style.text_scale * 2), style.margin, label_left);
    TEST_ASSERT_TRUE(label_rows > 3 * 6 * style.text_scale);  // three lines of them
    int t0, t1;
    rows_with(style.title, &t0, &t1);
    for (int y = t0 + font.height * style.title_scale; y <= t1; y++) {  // below the title: the values
        for (int x = 0; x < W; x++) {
            if (at(x, y) == style.title && x < value_left) {
                value_left = x;
            }
        }
    }
    TEST_ASSERT_INT_WITHIN((unsigned)(style.text_scale * 2), value_x, value_left);
    // The hint: label-coloured ink in the bottom eighth, and nothing else there.
    int hint = 0;
    for (int y = H - H / 8; y < H; y++) {
        for (int x = 0; x < W; x++) {
            hint += at(x, y) == style.label;
            TEST_ASSERT_TRUE(at(x, y) == style.label || at(x, y) == style.background);
        }
    }
    TEST_ASSERT_TRUE(hint > 100);

    // A paragraph and rows together, and an empty line between, at every rotation.
    p3a_ui_content(&c, "Wi-Fi setup", -1, "Join this network with a phone.\n\nNetwork\tp3a-setup\nOr open\thttp://192.168.4.1");
    for (int r = 0; r < 360; r += 90) {
        fresh(r);
        ui_draw_screen(&canvas, &style, &c);
        guard_intact();
        TEST_ASSERT_TRUE(count(style.text) > 100 && count(style.label) > 100 && count(style.title) > 100);
    }
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
        printf("%s is not a font\n", UI_FONT_FILE);
        return 1;
    }
    pixels = malloc((size_t)W * H * 3 + 64);
    UNITY_BEGIN();
    RUN_TEST(test_the_built_in_font_opens_and_the_default_style_suits_the_panel);
    RUN_TEST(test_a_screen_is_title_then_text_then_bar_centred_on_the_background);
    RUN_TEST(test_no_words_can_push_anything_off_the_screen);
    RUN_TEST(test_a_banner_is_a_band_at_the_viewers_bottom_and_leaves_the_rest);
    RUN_TEST(test_rows_line_up_in_two_columns_and_the_hint_sits_at_the_bottom);
    const int failed = UNITY_END();
    free(pixels);
    return failed;
}
