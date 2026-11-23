// SPDX-License-Identifier: Apache-2.0
// The rules of a turn, driven as the scheduler drives them, with the player
// and the clock played by the test.
#include "sched_turn.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static sched_turn_t t;

static void test_it_moves_on_after_the_dwell_and_not_before(void)
{
    sched_turn_init(&t, 30);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 100000, 5));  // nothing on the panel yet
    TEST_ASSERT_EQUAL(-1, sched_turn_remaining_s(&t, 0));
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    sched_turn_shown(&t, 1000);
    TEST_ASSERT_EQUAL(30, sched_turn_remaining_s(&t, 1000));
    TEST_ASSERT_EQUAL(1, sched_turn_remaining_s(&t, 30999));
    TEST_ASSERT_FALSE(sched_turn_due(&t, 30999, 5));
    TEST_ASSERT_TRUE(sched_turn_due(&t, 31000, 5));
    TEST_ASSERT_FALSE(sched_turn_due(&t, 31000, 1));  // nothing to move on to
    TEST_ASSERT_FALSE(sched_turn_due(&t, 31000, 0));

    // While the next one loads, the timer does not fire again.
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 99000, 5));
    sched_turn_shown(&t, 99000);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 100000, 5));
    TEST_ASSERT_TRUE(sched_turn_due(&t, 129000, 5));
}

static void test_a_dwell_of_zero_never_moves_on_and_a_change_applies_to_what_is_showing(void)
{
    sched_turn_init(&t, 0);
    sched_turn_begin(&t, SCHED_TURN_ANY);
    sched_turn_shown(&t, 0);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 86400000, 5));
    sched_turn_set_dwell(&t, 10);
    TEST_ASSERT_TRUE(sched_turn_due(&t, 10000, 5));  // it has had its ten seconds already
    sched_turn_set_dwell(&t, 60);
    TEST_ASSERT_EQUAL(50, sched_turn_remaining_s(&t, 10000));
}

static void test_pause_and_screens_stop_the_count_and_give_a_full_dwell_after(void)
{
    sched_turn_init(&t, 30);
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    sched_turn_shown(&t, 0);
    sched_turn_set_paused(&t, true, 20000);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 500000, 5));
    TEST_ASSERT_EQUAL(-1, sched_turn_remaining_s(&t, 500000));
    sched_turn_set_paused(&t, false, 500000);
    TEST_ASSERT_EQUAL(30, sched_turn_remaining_s(&t, 500000));
    TEST_ASSERT_TRUE(sched_turn_due(&t, 530000, 5));

    sched_turn_hold(&t);
    sched_turn_hold(&t);
    sched_turn_release(&t, 600000);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 700000, 5));  // one screen is still up
    sched_turn_release(&t, 700000);
    sched_turn_release(&t, 700000);  // one too many: harmless
    TEST_ASSERT_EQUAL(30, sched_turn_remaining_s(&t, 700000));
}

static void test_an_automatic_turn_tries_others_then_waits_quietly(void)
{
    sched_turn_init(&t, 30);
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    sched_turn_shown(&t, 0);
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    for (int i = 0; i < SCHED_TURN_RETRIES; i++) {
        const sched_turn_verdict_t v = sched_turn_failed(&t, SCHED_FAIL_NOT_PLAYABLE, 31000);
        TEST_ASSERT_TRUE(v.retry);
        TEST_ASSERT_FALSE(v.report);
        TEST_ASSERT_TRUE(v.set_aside);
        TEST_ASSERT_FALSE(v.delete_blob);
    }
    const sched_turn_verdict_t last = sched_turn_failed(&t, SCHED_FAIL_MISSING, 32000);
    TEST_ASSERT_FALSE(last.retry);
    TEST_ASSERT_FALSE(last.report);
    // What was on the panel stays for another dwell, then it tries again.
    TEST_ASSERT_FALSE(sched_turn_due(&t, 61000, 5));
    TEST_ASSERT_TRUE(sched_turn_due(&t, 62000, 5));
}

static void test_an_empty_panel_is_tried_again_after_a_dwell(void)
{
    sched_turn_init(&t, 30);
    sched_turn_begin(&t, SCHED_TURN_AUTO);  // the first artwork after boot
    for (int i = 0; i < SCHED_TURN_RETRIES; i++) {
        TEST_ASSERT_TRUE(sched_turn_failed(&t, SCHED_FAIL_CORRUPT, 1000).retry);
    }
    TEST_ASSERT_FALSE(sched_turn_failed(&t, SCHED_FAIL_CORRUPT, 1000).retry);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 30999, 1));
    TEST_ASSERT_TRUE(sched_turn_due(&t, 31000, 1));  // one artwork is enough when nothing is showing
    TEST_ASSERT_FALSE(sched_turn_due(&t, 31000, 0));

    // Not when something that is not the scheduler's took the panel.
    sched_turn_stopped(&t);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 99000, 5));
    // And not with no dwell: such a device moves only when asked.
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    sched_turn_gave_up(&t, 0);
    sched_turn_set_dwell(&t, 0);
    TEST_ASSERT_FALSE(sched_turn_due(&t, 99000, 5));
}

static void test_next_tries_others_then_reports_and_one_named_artwork_reports_at_once(void)
{
    sched_turn_init(&t, 30);
    sched_turn_begin(&t, SCHED_TURN_ANY);
    for (int i = 0; i < SCHED_TURN_RETRIES; i++) {
        TEST_ASSERT_TRUE(sched_turn_failed(&t, SCHED_FAIL_OTHER, 0).retry);
    }
    sched_turn_verdict_t v = sched_turn_failed(&t, SCHED_FAIL_OTHER, 0);
    TEST_ASSERT_FALSE(v.retry);
    TEST_ASSERT_TRUE(v.report);
    TEST_ASSERT_FALSE(v.set_aside);  // nothing was wrong with the artwork itself

    sched_turn_begin(&t, SCHED_TURN_THIS);
    v = sched_turn_failed(&t, SCHED_FAIL_MISSING, 0);
    TEST_ASSERT_FALSE(v.retry);
    TEST_ASSERT_TRUE(v.report);

    // A new turn starts with a fresh allowance.
    sched_turn_begin(&t, SCHED_TURN_ANY);
    TEST_ASSERT_TRUE(sched_turn_failed(&t, SCHED_FAIL_MISSING, 0).retry);
}

static void test_damaged_blobs_are_deleted_at_most_one_a_minute(void)
{
    sched_turn_init(&t, 30);
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    TEST_ASSERT_TRUE(sched_turn_failed(&t, SCHED_FAIL_CORRUPT, 5000).delete_blob);
    sched_turn_verdict_t v = sched_turn_failed(&t, SCHED_FAIL_CORRUPT, 6000);
    TEST_ASSERT_FALSE(v.delete_blob);
    TEST_ASSERT_TRUE(v.set_aside);  // still not offered again
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    TEST_ASSERT_FALSE(sched_turn_failed(&t, SCHED_FAIL_CORRUPT, 64999).delete_blob);
    TEST_ASSERT_TRUE(sched_turn_failed(&t, SCHED_FAIL_CORRUPT, 65000).delete_blob);
    TEST_ASSERT_FALSE(sched_turn_failed(&t, SCHED_FAIL_MISSING, 200000).delete_blob);
}

static void test_filtered_artworks_are_passed_over_a_bounded_number_of_times(void)
{
    sched_turn_init(&t, 30);
    TEST_ASSERT_FALSE(sched_turn_skip(&t));  // no turn under way
    sched_turn_begin(&t, SCHED_TURN_AUTO);
    for (int i = 0; i < SCHED_TURN_SKIPS; i++) {
        TEST_ASSERT_TRUE(sched_turn_skip(&t));
    }
    TEST_ASSERT_FALSE(sched_turn_skip(&t));
    // Passing over costs no retries.
    TEST_ASSERT_TRUE(sched_turn_failed(&t, SCHED_FAIL_MISSING, 0).retry);
    sched_turn_begin(&t, SCHED_TURN_THIS);
    TEST_ASSERT_FALSE(sched_turn_skip(&t));  // a named artwork is shown whatever its shape
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_it_moves_on_after_the_dwell_and_not_before);
    RUN_TEST(test_a_dwell_of_zero_never_moves_on_and_a_change_applies_to_what_is_showing);
    RUN_TEST(test_pause_and_screens_stop_the_count_and_give_a_full_dwell_after);
    RUN_TEST(test_an_automatic_turn_tries_others_then_waits_quietly);
    RUN_TEST(test_an_empty_panel_is_tried_again_after_a_dwell);
    RUN_TEST(test_next_tries_others_then_reports_and_one_named_artwork_reports_at_once);
    RUN_TEST(test_damaged_blobs_are_deleted_at_most_one_a_minute);
    RUN_TEST(test_filtered_artworks_are_passed_over_a_bounded_number_of_times);
    return UNITY_END();
}
