// SPDX-License-Identifier: Apache-2.0
// Where a picture goes on the panel, the lookup tables the upscaler reads,
// and telling file formats apart by their first bytes.
#include <string.h>

#include "player_fit.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static player_fit_t fit(int w, int h, bool quarter)
{
    player_fit_t f;
    player_fit_compute(w, h, 720, 720, quarter, &f);
    return f;
}

static void test_a_square_picture_fills_a_square_panel(void)
{
    player_fit_t f = fit(64, 64, false);
    TEST_ASSERT_EQUAL(720, f.scaled_w);
    TEST_ASSERT_EQUAL(720, f.scaled_h);
    TEST_ASSERT_EQUAL(0, f.offset_x);
    TEST_ASSERT_EQUAL(0, f.offset_y);
    TEST_ASSERT_FALSE(f.has_borders);
}

static void test_wide_and_tall_pictures_keep_their_shape_and_are_centred(void)
{
    player_fit_t wide = fit(240, 180, false);
    TEST_ASSERT_EQUAL(720, wide.scaled_w);
    TEST_ASSERT_EQUAL(540, wide.scaled_h);
    TEST_ASSERT_EQUAL(0, wide.offset_x);
    TEST_ASSERT_EQUAL(90, wide.offset_y);
    TEST_ASSERT_TRUE(wide.has_borders);

    player_fit_t tall = fit(96, 160, false);
    TEST_ASSERT_EQUAL(432, tall.scaled_w);
    TEST_ASSERT_EQUAL(720, tall.scaled_h);
    TEST_ASSERT_EQUAL(144, tall.offset_x);
    TEST_ASSERT_EQUAL(0, tall.offset_y);
}

static void test_a_quarter_turn_swaps_the_layout_and_the_table_lengths(void)
{
    player_fit_t f = fit(240, 180, true);  // now taller than wide on the panel
    TEST_ASSERT_EQUAL(540, f.scaled_w);
    TEST_ASSERT_EQUAL(720, f.scaled_h);
    TEST_ASSERT_EQUAL(90, f.offset_x);
    // lookup_x still maps to source columns, but is walked down the panel.
    TEST_ASSERT_EQUAL(720, f.lookup_x_len);
    TEST_ASSERT_EQUAL(540, f.lookup_y_len);
}

static void test_extreme_shapes_never_scale_to_nothing_or_past_the_panel(void)
{
    const int sizes[][2] = {{1, 1}, {4096, 1}, {1, 4096}, {4096, 4096}, {4095, 4096}, {3, 2}, {721, 720}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int quarter = 0; quarter < 2; quarter++) {
            player_fit_t f = fit(sizes[i][0], sizes[i][1], quarter);
            TEST_ASSERT_TRUE(f.scaled_w >= 1 && f.scaled_w <= 720);
            TEST_ASSERT_TRUE(f.scaled_h >= 1 && f.scaled_h <= 720);
            TEST_ASSERT_TRUE(f.offset_x >= 0 && f.offset_x + f.scaled_w <= 720);
            TEST_ASSERT_TRUE(f.offset_y >= 0 && f.offset_y + f.scaled_h <= 720);
            TEST_ASSERT_TRUE(f.scaled_w == 720 || f.scaled_h == 720);
            TEST_ASSERT_TRUE(f.lookup_x_len >= 1 && f.lookup_x_len <= 720);
            TEST_ASSERT_TRUE(f.lookup_y_len >= 1 && f.lookup_y_len <= 720);
        }
    }
}

static void test_lookup_entries_stay_inside_the_source_and_cover_it(void)
{
    uint16_t table[720];
    const int cases[][2] = {{720, 90}, {540, 37}, {720, 4096}, {1, 300}, {720, 1}, {433, 720}};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const int used = cases[c][0], src = cases[c][1];
        memset(table, 0xFF, sizeof(table));
        player_fit_fill_lookup(table, 720, used, src);
        TEST_ASSERT_EQUAL(0, table[0]);
        for (int i = 0; i < 720; i++) {
            TEST_ASSERT_TRUE(table[i] < src);
            if (i > 0) {
                TEST_ASSERT_TRUE(table[i] >= table[i - 1]);
            }
            if (i < used) {
                TEST_ASSERT_EQUAL(i * src / used, table[i]);
            } else {
                TEST_ASSERT_EQUAL(table[used - 1], table[i]);  // the tail repeats the last entry
            }
        }
    }
}

static void test_lookup_refuses_lengths_that_would_write_outside_the_table(void)
{
    uint16_t table[8];
    memset(table, 0xAB, sizeof(table));
    player_fit_fill_lookup(table, 4, 9999, 10);  // used is clamped to the table
    TEST_ASSERT_EQUAL(0, table[0]);
    TEST_ASSERT_EQUAL(7, table[3]);
    TEST_ASSERT_EQUAL(0xABAB, table[4]);
    player_fit_fill_lookup(table, 4, 0, 10);     // nothing used: one entry, repeated
    TEST_ASSERT_EQUAL(0, table[3]);
    player_fit_fill_lookup(table, 0, 4, 10);     // no table
    player_fit_fill_lookup(table, 4, 4, 0);      // no source
    TEST_ASSERT_EQUAL(0xABAB, table[4]);
}

static void test_formats_are_told_apart_by_their_first_bytes(void)
{
    const uint8_t webp[] = {'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'E', 'B', 'P'};
    const uint8_t wav[] = {'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'A', 'V', 'E'};
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_WEBP, player_format_sniff(webp, sizeof(webp)));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_UNKNOWN, player_format_sniff(wav, sizeof(wav)));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_GIF, player_format_sniff((const uint8_t *)"GIF89a", 6));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_GIF, player_format_sniff((const uint8_t *)"GIF87a", 6));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_PNG, player_format_sniff(png, sizeof(png)));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_JPEG, player_format_sniff(jpeg, sizeof(jpeg)));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_BMP, player_format_sniff((const uint8_t *)"BM", 2));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_UNKNOWN, player_format_sniff((const uint8_t *)"<html>", 6));
}

static void test_short_and_missing_input_is_unknown_not_read_past(void)
{
    const uint8_t webp[] = {'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'E', 'B', 'P'};
    for (size_t n = 0; n < sizeof(webp); n++) {
        // Under the sanitizers, a read past n bytes of an n-byte copy fails the test.
        uint8_t copy[sizeof(webp)];
        memcpy(copy, webp, n);
        TEST_ASSERT_EQUAL(PLAYER_FORMAT_UNKNOWN, player_format_sniff(n ? copy : NULL, n));
    }
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_UNKNOWN, player_format_sniff((const uint8_t *)"GIF89", 5));
    TEST_ASSERT_EQUAL(PLAYER_FORMAT_UNKNOWN, player_format_sniff((const uint8_t *)"B", 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_square_picture_fills_a_square_panel);
    RUN_TEST(test_wide_and_tall_pictures_keep_their_shape_and_are_centred);
    RUN_TEST(test_a_quarter_turn_swaps_the_layout_and_the_table_lengths);
    RUN_TEST(test_extreme_shapes_never_scale_to_nothing_or_past_the_panel);
    RUN_TEST(test_lookup_entries_stay_inside_the_source_and_cover_it);
    RUN_TEST(test_lookup_refuses_lengths_that_would_write_outside_the_table);
    RUN_TEST(test_formats_are_told_apart_by_their_first_bytes);
    RUN_TEST(test_short_and_missing_input_is_unknown_not_read_past);
    return UNITY_END();
}
