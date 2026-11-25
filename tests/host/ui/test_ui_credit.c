// SPDX-License-Identifier: Apache-2.0
// Where a source's credit goes under an artwork.
#include "ui_credit.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define TEXT_W 96  // "Powered by GIPHY" is about this wide at scale 1
#define TEXT_H 9

static void test_a_wide_artwork_has_its_credit_centred_in_the_band_below(void)
{
    // 480x270 on 720x720: shown 720x405, from row 157 to 562. The band below is 158 rows.
    const ui_credit_place_t p = ui_credit_place(480, 270, 720, 720, TEXT_W, TEXT_H);
    TEST_ASSERT_TRUE(p.show);
    TEST_ASSERT_EQUAL(3, p.scale);
    TEST_ASSERT_EQUAL((720 - 96 * 3) / 2, p.x);
    TEST_ASSERT_EQUAL(562 + (158 - 27) / 2, p.y);
    TEST_ASSERT_TRUE(p.y >= 562 + UI_CREDIT_MARGIN && p.y + 27 <= 720 - UI_CREDIT_MARGIN);
}

static void test_no_band_or_too_low_a_band_means_no_credit(void)
{
    TEST_ASSERT_FALSE(ui_credit_place(500, 500, 720, 720, TEXT_W, TEXT_H).show);  // fills the screen
    TEST_ASSERT_FALSE(ui_credit_place(300, 500, 720, 720, TEXT_W, TEXT_H).show);  // tall: bands at the sides only
    // 720x690 leaves 15 rows below: too low for 18 rows of text and its margins.
    TEST_ASSERT_FALSE(ui_credit_place(720, 690, 720, 720, TEXT_W, TEXT_H).show);
    // 720x668: 26 rows below, exactly the text at twice its size and four rows each side.
    const ui_credit_place_t tight = ui_credit_place(720, 668, 720, 720, TEXT_W, TEXT_H);
    TEST_ASSERT_TRUE(tight.show);
    TEST_ASSERT_EQUAL(2, tight.scale);
    TEST_ASSERT_EQUAL(694 + 4, tight.y);
    TEST_ASSERT_FALSE(ui_credit_place(720, 670, 720, 720, TEXT_W, TEXT_H).show);  // 25 rows: one short
    // 35 rows: room for scale 3 (27 rows and margins).
    TEST_ASSERT_EQUAL(3, ui_credit_place(720, 650, 720, 720, TEXT_W, TEXT_H).scale);
}

static void test_a_long_line_is_drawn_smaller_or_not_at_all(void)
{
    // 200 wide: 600 at scale 3 is more than four fifths of 720; 400 at scale 2 fits.
    TEST_ASSERT_EQUAL(2, ui_credit_place(480, 270, 720, 720, 200, TEXT_H).scale);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 300, TEXT_H).show);
}

static void test_nonsense_sizes_show_nothing(void)
{
    TEST_ASSERT_FALSE(ui_credit_place(0, 270, 720, 720, TEXT_W, TEXT_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, -1, 720, 720, TEXT_W, TEXT_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 0, TEXT_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 0, 0, TEXT_W, TEXT_H).show);
    // A sliver of a picture: shown one row high, the credit still below it.
    const ui_credit_place_t p = ui_credit_place(65535, 1, 720, 720, TEXT_W, TEXT_H);
    TEST_ASSERT_TRUE(p.show && p.y > 360);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_wide_artwork_has_its_credit_centred_in_the_band_below);
    RUN_TEST(test_no_band_or_too_low_a_band_means_no_credit);
    RUN_TEST(test_a_long_line_is_drawn_smaller_or_not_at_all);
    RUN_TEST(test_nonsense_sizes_show_nothing);
    return UNITY_END();
}
