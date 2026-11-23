// SPDX-License-Identifier: Apache-2.0
// What each gesture does on each screen.
#include "ui_route.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ui_action_t route(p3a_screen_t screen, bool paused, ui_gesture_kind_t kind, int x)
{
    const ui_gesture_t g = {.kind = kind, .x = x, .y = 100};
    return ui_route(screen, paused, &g, 720);
}

static const ui_gesture_kind_t all[] = {UI_GESTURE_TAP,        UI_GESTURE_SWIPE_UP,  UI_GESTURE_SWIPE_DOWN,
                                        UI_GESTURE_LONG_PRESS, UI_GESTURE_ROTATE_CW, UI_GESTURE_ROTATE_CCW};

static void test_on_the_artwork_every_gesture_has_its_meaning(void)
{
    TEST_ASSERT_EQUAL(UI_ACTION_BACK, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_TAP, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_BACK, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_TAP, 359));
    TEST_ASSERT_EQUAL(UI_ACTION_NEXT, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_TAP, 360));
    TEST_ASSERT_EQUAL(UI_ACTION_NEXT, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_TAP, 719));
    TEST_ASSERT_EQUAL(UI_ACTION_LIKE, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_SWIPE_UP, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_UNLIKE, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_SWIPE_DOWN, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_TURN_CW, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_ROTATE_CW, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_TURN_CCW, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_ROTATE_CCW, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_OPEN_INFO, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_LONG_PRESS, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, route(P3A_SCREEN_ARTWORK, false, UI_GESTURE_NONE, 0));
}

static void test_while_paused_only_a_long_press_works_and_it_resumes(void)
{
    for (unsigned i = 0; i < 6; i++) {
        const ui_action_t want = all[i] == UI_GESTURE_LONG_PRESS ? UI_ACTION_RESUME : UI_ACTION_NONE;
        TEST_ASSERT_EQUAL(want, route(P3A_SCREEN_ARTWORK, true, all[i], 500));
        TEST_ASSERT_EQUAL(want, route(P3A_SCREEN_NOTICE, true, all[i], 500));
    }
}

static void test_a_long_press_closes_the_screens_a_person_may_close_and_nothing_else_reaches_them(void)
{
    const p3a_screen_t closable[] = {P3A_SCREEN_INFO, P3A_SCREEN_SETUP, P3A_SCREEN_REGISTRATION};
    for (unsigned s = 0; s < 3; s++) {
        for (unsigned i = 0; i < 6; i++) {
            const ui_action_t want = all[i] == UI_GESTURE_LONG_PRESS ? UI_ACTION_CLOSE_SCREEN : UI_ACTION_NONE;
            TEST_ASSERT_EQUAL(want, route(closable[s], false, all[i], 500));
            TEST_ASSERT_EQUAL(want, route(closable[s], true, all[i], 500));  // paused or not
        }
    }
}

static void test_an_update_a_shared_card_and_a_fatal_error_are_not_interrupted(void)
{
    const p3a_screen_t deaf[] = {P3A_SCREEN_UPDATE, P3A_SCREEN_USB, P3A_SCREEN_FATAL};
    for (unsigned s = 0; s < 3; s++) {
        for (unsigned i = 0; i < 6; i++) {
            TEST_ASSERT_EQUAL(UI_ACTION_NONE, route(deaf[s], false, all[i], 500));
        }
    }
}

static void test_a_notice_that_fills_the_screen_leaves_the_device_usable(void)
{
    TEST_ASSERT_EQUAL(UI_ACTION_OPEN_INFO, route(P3A_SCREEN_NOTICE, false, UI_GESTURE_LONG_PRESS, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_TURN_CW, route(P3A_SCREEN_NOTICE, false, UI_GESTURE_ROTATE_CW, 0));
    // Nothing is playing: there is nothing to step through or to like.
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, route(P3A_SCREEN_NOTICE, false, UI_GESTURE_TAP, 500));
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, route(P3A_SCREEN_NOTICE, false, UI_GESTURE_SWIPE_UP, 500));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_on_the_artwork_every_gesture_has_its_meaning);
    RUN_TEST(test_while_paused_only_a_long_press_works_and_it_resumes);
    RUN_TEST(test_a_long_press_closes_the_screens_a_person_may_close_and_nothing_else_reaches_them);
    RUN_TEST(test_an_update_a_shared_card_and_a_fatal_error_are_not_interrupted);
    RUN_TEST(test_a_notice_that_fills_the_screen_leaves_the_device_usable);
    return UNITY_END();
}
