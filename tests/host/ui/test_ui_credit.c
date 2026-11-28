// SPDX-License-Identifier: Apache-2.0
// Where a source's mark goes under an artwork.
#include "ui_clock.h"
#include "ui_credit.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define MARK_W 91  // Giphy's mark
#define MARK_H 24

static void test_a_wide_artwork_has_its_mark_at_the_right_of_the_band_below(void)
{
    // 480x270 on 720x720: shown 720x405, from row 157 to 562. The band below is 158 rows.
    const ui_credit_place_t p = ui_credit_place(480, 270, 720, 720, MARK_W, MARK_H, false);
    TEST_ASSERT_TRUE(p.show);
    TEST_ASSERT_EQUAL(720 - UI_CREDIT_SIDE_MARGIN - MARK_W, p.x);
    TEST_ASSERT_EQUAL(562 + (158 - 24) / 2, p.y);
    // A wider mark ends at the same column.
    const ui_credit_place_t wider = ui_credit_place(480, 270, 720, 720, 126, MARK_H, false);
    TEST_ASSERT_EQUAL(p.x + MARK_W, wider.x + 126);
    TEST_ASSERT_EQUAL(p.y, wider.y);
    // The screen turned on its side is as wide as it is high here; a screen
    // that is not square places the mark by its own width.
    TEST_ASSERT_EQUAL(1000 - UI_CREDIT_SIDE_MARGIN - MARK_W, ui_credit_place(480, 270, 1000, 720, MARK_W, MARK_H, false).x);
}

static void test_no_band_or_too_low_a_band_means_no_mark(void)
{
    TEST_ASSERT_FALSE(ui_credit_place(500, 500, 720, 720, MARK_W, MARK_H, false).show);  // fills the screen
    TEST_ASSERT_FALSE(ui_credit_place(300, 500, 720, 720, MARK_W, MARK_H, false).show);  // tall: bands at the sides only
    // 720x690 leaves 30 rows below: too low for 24 rows and four each side.
    TEST_ASSERT_FALSE(ui_credit_place(720, 690, 720, 720, MARK_W, MARK_H, false).show);
    // 720x656: 32 rows below, exactly the mark and its margins.
    const ui_credit_place_t tight = ui_credit_place(720, 656, 720, 720, MARK_W, MARK_H, false);
    TEST_ASSERT_TRUE(tight.show);
    TEST_ASSERT_EQUAL(688 + UI_CREDIT_MARGIN, tight.y);
    TEST_ASSERT_FALSE(ui_credit_place(720, 658, 720, 720, MARK_W, MARK_H, false).show);  // 31 rows: one short
    // A higher mark needs a higher band.
    TEST_ASSERT_FALSE(ui_credit_place(720, 656, 720, 720, MARK_W, 36, false).show);
    TEST_ASSERT_TRUE(ui_credit_place(720, 632, 720, 720, MARK_W, 36, false).show);  // 44 rows
}

static void test_a_mark_wider_than_the_screen_allows_is_not_drawn(void)
{
    TEST_ASSERT_TRUE(ui_credit_place(480, 270, 720, 720, 720 - 2 * UI_CREDIT_SIDE_MARGIN, MARK_H, false).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 720 - 2 * UI_CREDIT_SIDE_MARGIN + 1, MARK_H, false).show);
}

static void test_nonsense_sizes_show_nothing(void)
{
    TEST_ASSERT_FALSE(ui_credit_place(0, 270, 720, 720, MARK_W, MARK_H, false).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, -1, 720, 720, MARK_W, MARK_H, false).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 0, MARK_H, false).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, MARK_W, 0, false).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 0, 0, MARK_W, MARK_H, false).show);
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, INT32_MAX, INT32_MAX, false).show);
    // A sliver of a picture: shown one row high, the mark still below it.
    const ui_credit_place_t p = ui_credit_place(65535, 1, 720, 720, MARK_W, MARK_H, false);
    TEST_ASSERT_TRUE(p.show && p.y > 360);
}

static void test_the_mark_goes_left_when_the_clock_is_where_it_would_be(void)
{
    TEST_ASSERT_TRUE(ui_credit_at_left(true, UI_CLOCK_BOTTOM_RIGHT));
    TEST_ASSERT_FALSE(ui_credit_at_left(false, UI_CLOCK_BOTTOM_RIGHT));  // the clock is off
    for (int place = UI_CLOCK_TOP_LEFT; place <= UI_CLOCK_BOTTOM_RIGHT; place++) {
        TEST_ASSERT_EQUAL(place == UI_CLOCK_BOTTOM_RIGHT, ui_credit_at_left(true, place));
    }
    const ui_credit_place_t right = ui_credit_place(480, 270, 720, 720, MARK_W, MARK_H, false);
    const ui_credit_place_t left = ui_credit_place(480, 270, 720, 720, MARK_W, MARK_H, true);
    TEST_ASSERT_TRUE(left.show);
    TEST_ASSERT_EQUAL(UI_CREDIT_SIDE_MARGIN, left.x);
    TEST_ASSERT_EQUAL(right.y, left.y);
    // The same room is asked for on either side.
    TEST_ASSERT_FALSE(ui_credit_place(480, 270, 720, 720, 720 - 2 * UI_CREDIT_SIDE_MARGIN + 1, MARK_H, true).show);
    TEST_ASSERT_FALSE(ui_credit_place(500, 500, 720, 720, MARK_W, MARK_H, true).show);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_wide_artwork_has_its_mark_at_the_right_of_the_band_below);
    RUN_TEST(test_no_band_or_too_low_a_band_means_no_mark);
    RUN_TEST(test_a_mark_wider_than_the_screen_allows_is_not_drawn);
    RUN_TEST(test_nonsense_sizes_show_nothing);
    RUN_TEST(test_the_mark_goes_left_when_the_clock_is_where_it_would_be);
    return UNITY_END();
}
