// SPDX-License-Identifier: Apache-2.0
// Where a source's mark goes under an artwork.
#include "ui_credit.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define MARK_W 100  // Giphy's mark
#define MARK_H 27

static void test_a_wide_artwork_has_its_mark_at_the_right_of_the_band_below(void)
{
    // 480x270 on 720x720: shown 720x405, from row 157 to 562. The band below is 158 rows.
    const ui_credit_place_t p = ui_credit_place(480, 270, 720, 720, MARK_W, MARK_H);
    TEST_ASSERT_TRUE(p.show);
    TEST_ASSERT_EQUAL(720 - UI_CREDIT_RIGHT_MARGIN - MARK_W, p.x);
    TEST_ASSERT_EQUAL(562 + (158 - 27) / 2, p.y);
    // A wider mark ends at the same column.
    const ui_credit_place_t wider = ui_credit_place(480, 270, 720, 720, 126, MARK_H);
    TEST_ASSERT_EQUAL(p.x + MARK_W, wider.x + 126);
    TEST_ASSERT_EQUAL(p.y, wider.y);
    // The screen turned on its side is as wide as it is high here; a screen
    // that is not square places the mark by its own width.
    TEST_ASSERT_EQUAL(1000 - UI_CREDIT_RIGHT_MARGIN - MARK_W, ui_credit_place(480, 270, 1000, 720, MARK_W, MARK_H).x);
}

static void test_no_band_or_too_low_a_band_means_no_mark(void)
{
    TEST_ASSERT_FALSE(ui_credit_place(500, 500, 720, 720, MARK_W, MARK_H).show);  // fills the screen
    TEST_ASSERT_FALSE(ui_credit_place(300, 500, 720, 720, MARK_W, MARK_H).show);  // tall: bands at the sides only
    // 720x660 leaves 30 rows below: too low for 27 rows and four each side.
    TEST_ASSERT_FALSE(ui_credit_place(720, 660, 720, 720, MARK_W, MARK_H).show);
    // 720x650: 35 rows below, exactly the mark and its margins.
    const ui_credit_place_t tight = ui_credit_place(720, 650, 720, 720, MARK_W, MARK_H);
    TEST_ASSERT_TRUE(tight.show);
    TEST_ASSERT_EQUAL(685 + UI_CREDIT_MARGIN, tight.y);
    TEST_ASSERT_FALSE(ui_credit_place(720, 652, 720, 720, MARK_W, MARK_H).show);  // 34 rows: one short
}

static void test_a_mark_wider_than_the_screen_allows_is_not_drawn(void)
{
    TEST_ASSERT_TRUE(ui_credit_place(480, 270, 720, 720, 720 - 2 * UI_CREDIT_RIGHT_MARGIN, MARK_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 720 - 2 * UI_CREDIT_RIGHT_MARGIN + 1, MARK_H).show);
}

static void test_nonsense_sizes_show_nothing(void)
{
    TEST_ASSERT_FALSE(ui_credit_place(0, 270, 720, 720, MARK_W, MARK_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, -1, 720, 720, MARK_W, MARK_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 0, MARK_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, MARK_W, 0).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 0, 0, MARK_W, MARK_H).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, INT32_MAX, INT32_MAX).show);
    // A sliver of a picture: shown one row high, the mark still below it.
    const ui_credit_place_t p = ui_credit_place(65535, 1, 720, 720, MARK_W, MARK_H);
    TEST_ASSERT_TRUE(p.show && p.y > 360);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_wide_artwork_has_its_mark_at_the_right_of_the_band_below);
    RUN_TEST(test_no_band_or_too_low_a_band_means_no_mark);
    RUN_TEST(test_a_mark_wider_than_the_screen_allows_is_not_drawn);
    RUN_TEST(test_nonsense_sizes_show_nothing);
    return UNITY_END();
}
