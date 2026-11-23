// SPDX-License-Identifier: Apache-2.0
// The UI's small pictures: the file format and drawing.
#include <stdlib.h>
#include <string.h>

#include "ui_image.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 16
#define H 12

static uint8_t pixels[W * H * 3 + 16];
static ui_canvas_t canvas;

static void fresh(int rotation)
{
    memset(pixels, 0, sizeof(pixels));
    memset(pixels + W * H * 3, 0xEE, 16);
    canvas = (ui_canvas_t){.pixels = pixels, .width = W, .height = H, .stride = W * 3, .rotation = rotation};
}

static uint32_t at(int x, int y)
{
    const uint8_t *p = pixels + (y * W + x) * 3;
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

// 3 by 2: red, clear, blue / green, green, half-clear white (not drawn).
static size_t tiny(uint8_t *out)
{
    static const uint8_t file[] = {
        'P', '3', 'A', 'I', 1, 0, 3, 0, 2, 0,
        0x00, 0x00, 0xFF, 0xFF, 0x12, 0x34, 0x56, 0x00, 0xFF, 0x00, 0x00, 0xFF,
        0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F,
    };
    memcpy(out, file, sizeof(file));
    return sizeof(file);
}

static void test_a_picture_is_drawn_pixel_for_pixel_and_clear_pixels_are_left(void)
{
    uint8_t bytes[64];
    ui_image_t image;
    TEST_ASSERT_TRUE(ui_image_open(&image, bytes, tiny(bytes)));
    TEST_ASSERT_EQUAL(3, image.width);
    TEST_ASSERT_EQUAL(2, image.height);
    fresh(0);
    ui_image_draw(&canvas, &image, 2, 1, 1);
    TEST_ASSERT_EQUAL(4, lit());
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, at(2, 1));  // red
    TEST_ASSERT_EQUAL_HEX32(0x000000, at(3, 1));  // clear
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, at(4, 1));  // blue
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, at(2, 2));
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, at(3, 2));  // alpha 128 is drawn
    TEST_ASSERT_EQUAL_HEX32(0x000000, at(4, 2));  // alpha 127 is not

    fresh(0);
    ui_image_draw(&canvas, &image, 0, 0, 3);
    TEST_ASSERT_EQUAL(4 * 9, lit());
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, at(8, 2));

    // Upright for the viewer when the device is turned: the red pixel, the
    // picture's top left, is at the viewer's top left, the panel's top right.
    fresh(90);
    ui_image_draw(&canvas, &image, 0, 0, 1);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, at(W - 1, 0));
    TEST_ASSERT_EQUAL(4, lit());

    // Off the edges: clipped.
    fresh(0);
    ui_image_draw(&canvas, &image, W - 1, H - 1, 4);
    ui_image_draw(&canvas, &image, -100, -100, 4);
    ui_image_draw(&canvas, &image, 0, 0, 0);
    TEST_ASSERT_EQUAL(1, lit());
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xEE, pixels[W * H * 3 + i]);
    }
}

static void test_bytes_that_are_not_a_picture_are_refused(void)
{
    uint8_t bytes[64];
    const size_t len = tiny(bytes);
    ui_image_t image;
    TEST_ASSERT_FALSE(ui_image_open(&image, NULL, len));
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, 9));
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, len - 1));
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, len + 1));
    bytes[3] = 'F';
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, len));
    tiny(bytes);
    bytes[4] = 2;
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, len));
    tiny(bytes);
    bytes[6] = 0;  // no width
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, len));
    tiny(bytes);
    bytes[7] = 0x40;  // a width of 16387
    TEST_ASSERT_FALSE(ui_image_open(&image, bytes, len));
    // Not opened: draws nothing.
    fresh(0);
    ui_image_draw(&canvas, &image, 0, 0, 1);
    TEST_ASSERT_EQUAL(0, lit());
}

// Any bytes: refused, or drawn without reading outside the file.
static void test_damaged_picture_files_are_refused_or_harmless(void)
{
    uint8_t good[64];
    const size_t len = tiny(good);
    uint32_t seed = 99;
    for (int round = 0; round < 20000; round++) {
        seed = seed * 1664525u + 1013904223u;
        const size_t cut = round % 3 == 0 ? (seed >> 8) % (len + 1) : len;
        uint8_t *bytes = malloc(cut ? cut : 1);
        memcpy(bytes, good, cut);
        if (cut > 0) {
            seed = seed * 1664525u + 1013904223u;
            bytes[(seed >> 8) % cut] = (uint8_t)(seed >> 24);
        }
        ui_image_t image;
        if (ui_image_open(&image, bytes, cut)) {
            fresh(round % 4 * 90);
            ui_image_draw(&canvas, &image, 1, 1, 2);
        }
        free(bytes);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_picture_is_drawn_pixel_for_pixel_and_clear_pixels_are_left);
    RUN_TEST(test_bytes_that_are_not_a_picture_are_refused);
    RUN_TEST(test_damaged_picture_files_are_refused_or_harmless);
    return UNITY_END();
}
