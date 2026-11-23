// SPDX-License-Identifier: Apache-2.0
// Gestures from touch samples, fed as the device feeds them: one sample
// every 20 ms.
#include <math.h>
#include <string.h>

#include "ui_gesture.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 720
#define H 720
#define STEP_MS 20

static ui_gesture_state_t g;
static int64_t now;
static int rotation;
static ui_gesture_t seen[16];
static int seen_count;

static void start(int turned)
{
    ui_gesture_init(&g, W, H);
    now = 1000;
    rotation = turned;
    seen_count = 0;
}

static void feed(const ui_touch_t *touch)
{
    const ui_gesture_t got = ui_gesture_feed(&g, touch, now, rotation);
    now += STEP_MS;
    if (got.kind != UI_GESTURE_NONE && seen_count < 16) {
        seen[seen_count++] = got;
    }
}

static void one(int x, int y)
{
    ui_touch_t t = {.count = 1, .x = {x, 0}, .y = {y, 0}};
    feed(&t);
}

static void two(int x0, int y0, int x1, int y1)
{
    ui_touch_t t = {.count = 2, .x = {x0, x1}, .y = {y0, y1}};
    feed(&t);
}

static void lift(void)
{
    ui_touch_t t = {0};
    feed(&t);
}

// One finger from (x0, y0) to (x1, y1) in `steps` samples, then lifted.
static void drag(int x0, int y0, int x1, int y1, int steps)
{
    for (int i = 0; i <= steps; i++) {
        one(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    }
    lift();
}

static void test_a_tap_is_reported_on_release_with_where_it_was(void)
{
    start(0);
    one(600, 100);
    one(602, 101);
    TEST_ASSERT_EQUAL(0, seen_count);  // not while the finger is down
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_TAP, seen[0].kind);
    TEST_ASSERT_EQUAL(600, seen[0].x);
    TEST_ASSERT_EQUAL(100, seen[0].y);
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);  // nothing more from an idle panel
}

static void test_a_tap_is_placed_as_the_viewer_sees_a_turned_screen(void)
{
    // The panel's top right corner is, turned 90 degrees, the viewer's top left.
    start(90);
    one(700, 10);
    lift();
    TEST_ASSERT_EQUAL(UI_GESTURE_TAP, seen[0].kind);
    TEST_ASSERT_EQUAL(10, seen[0].x);
    TEST_ASSERT_EQUAL(19, seen[0].y);

    start(180);
    one(700, 10);
    lift();
    TEST_ASSERT_EQUAL(19, seen[0].x);
    TEST_ASSERT_EQUAL(709, seen[0].y);

    start(270);
    one(700, 10);
    lift();
    TEST_ASSERT_EQUAL(709, seen[0].x);
    TEST_ASSERT_EQUAL(700, seen[0].y);
}

static void test_a_swipe_is_reported_when_a_quarter_of_the_screen_is_crossed(void)
{
    start(0);
    for (int y = 500; y > 330; y -= 10) {
        one(360, y);
        TEST_ASSERT_EQUAL(0, seen_count);  // 170 px so far: not yet
    }
    one(360, 320);  // 180 px
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_SWIPE_UP, seen[0].kind);
    one(360, 100);  // going on changes nothing
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);  // and the release is not a tap

    start(0);
    drag(360, 200, 360, 500, 15);
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_SWIPE_DOWN, seen[0].kind);
}

static void test_up_is_up_as_the_viewer_sees_it(void)
{
    // Turned 90 degrees, the viewer's "up" is the panel's +x.
    start(90);
    drag(200, 360, 500, 360, 15);
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_SWIPE_UP, seen[0].kind);
    // A sideways drag, as the viewer sees it, is no swipe and no tap.
    start(90);
    drag(360, 200, 360, 500, 15);
    TEST_ASSERT_EQUAL(0, seen_count);
}

static void test_a_touch_that_drifts_is_neither_a_tap_nor_a_swipe(void)
{
    start(0);
    drag(360, 400, 360, 300, 10);  // 100 px: past still (47), short of a swipe (180)
    TEST_ASSERT_EQUAL(0, seen_count);
    // Out and back to where it started: still not a tap.
    start(0);
    one(360, 400);
    one(360, 320);
    one(360, 400);
    lift();
    TEST_ASSERT_EQUAL(0, seen_count);
    // Within the still distance it is a tap.
    start(0);
    drag(360, 400, 380, 420, 4);  // 40 px
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_TAP, seen[0].kind);
}

static void test_a_long_press_is_reported_once_while_the_finger_is_down(void)
{
    start(0);
    for (int t = 0; t < UI_GESTURE_LONG_PRESS_MS; t += STEP_MS) {
        one(300, 300);
    }
    TEST_ASSERT_EQUAL(0, seen_count);
    one(303, 301);
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_LONG_PRESS, seen[0].kind);
    for (int i = 0; i < 200; i++) {
        one(300, 300);  // four more seconds
    }
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);  // once, and the release is not a tap

    // A finger that has wandered off is not pressing.
    start(0);
    one(300, 300);
    for (int t = 0; t <= UI_GESTURE_LONG_PRESS_MS + 500; t += STEP_MS) {
        one(380, 300);  // 80 px from where it came down
    }
    lift();
    TEST_ASSERT_EQUAL(0, seen_count);
}

// Two fingers on a circle of radius 150 around the centre, at `degrees`.
static void fingers_at(float degrees)
{
    const float a = degrees * 3.14159265f / 180.0f;
    two(360 + (int)(150 * cosf(a)), 360 + (int)(150 * sinf(a)), 360 - (int)(150 * cosf(a)), 360 - (int)(150 * sinf(a)));
}

static void test_two_fingers_turning_45_degrees_rotate_once(void)
{
    start(0);
    for (float d = 0; d <= 40; d += 4) {
        fingers_at(d);
    }
    TEST_ASSERT_EQUAL(0, seen_count);
    for (float d = 44; d <= 170; d += 4) {
        fingers_at(d);
    }
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);  // once, however far
    TEST_ASSERT_EQUAL(UI_GESTURE_ROTATE_CW, seen[0].kind);

    start(0);
    for (float d = 0; d >= -60; d -= 4) {
        fingers_at(d);
    }
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_ROTATE_CCW, seen[0].kind);

    // Through the point where the angle wraps around.
    start(0);
    for (float d = 160; d <= 215; d += 5) {
        fingers_at(d);
    }
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_ROTATE_CW, seen[0].kind);
}

static void test_fingers_lifted_one_after_the_other_leave_no_tap(void)
{
    start(0);
    one(200, 360);  // the first finger
    fingers_at(0);
    fingers_at(10);
    one(210, 360);  // the second lifted
    one(210, 360);
    lift();
    TEST_ASSERT_EQUAL(0, seen_count);
    // The next touch is judged afresh.
    one(100, 100);
    lift();
    TEST_ASSERT_EQUAL(1, seen_count);
    TEST_ASSERT_EQUAL(UI_GESTURE_TAP, seen[0].kind);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_tap_is_reported_on_release_with_where_it_was);
    RUN_TEST(test_a_tap_is_placed_as_the_viewer_sees_a_turned_screen);
    RUN_TEST(test_a_swipe_is_reported_when_a_quarter_of_the_screen_is_crossed);
    RUN_TEST(test_up_is_up_as_the_viewer_sees_it);
    RUN_TEST(test_a_touch_that_drifts_is_neither_a_tap_nor_a_swipe);
    RUN_TEST(test_a_long_press_is_reported_once_while_the_finger_is_down);
    RUN_TEST(test_two_fingers_turning_45_degrees_rotate_once);
    RUN_TEST(test_fingers_lifted_one_after_the_other_leave_no_tap);
    return UNITY_END();
}
