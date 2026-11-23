// SPDX-License-Identifier: Apache-2.0
// Fingers on the panel turned into gestures. Pure: no IDF includes, no
// clock of its own; host-tested (tests/host/ui/test_ui_gesture.c). Private
// to the UI.
//
// Fed one sample of the touch controller at a time (every 20 ms on the
// device), it answers with at most one gesture per sample:
//
//   tap           one finger down and up without moving; reported on
//                 release, with where it was as the viewer sees the screen
//   swipe up/down one finger moved a quarter of the screen vertically, as
//                 the viewer sees it; reported when the distance is reached
//   long press    one finger held still for 2.25 s; reported while down
//   rotate        two fingers turned 45 degrees; reported once per gesture
//
// A touch that drifts further than a tap may, and not far enough for a
// swipe, is nothing: an aborted swipe must not skip to the next artwork.
//
// The thresholds and the order of the checks are 1.x's (app_touch.c), which
// were tuned on the device with real fingers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_GESTURE_LONG_PRESS_MS   2250
#define UI_GESTURE_SWIPE_PERCENT   25   // of the screen's height
#define UI_GESTURE_STILL_PERMILLE  65   // of the screen's shorter side: how far a finger may wander and still be still
#define UI_GESTURE_ROTATE_DEGREES  45

typedef enum {
    UI_GESTURE_NONE = 0,
    UI_GESTURE_TAP,
    UI_GESTURE_SWIPE_UP,
    UI_GESTURE_SWIPE_DOWN,
    UI_GESTURE_LONG_PRESS,
    UI_GESTURE_ROTATE_CW,
    UI_GESTURE_ROTATE_CCW,
} ui_gesture_kind_t;

typedef struct {
    ui_gesture_kind_t kind;
    int32_t x, y;  // of a tap, as the viewer sees the screen (the rotation applied)
} ui_gesture_t;

// What the touch controller reports: how many fingers, and the first two,
// in the panel's own coordinates.
typedef struct {
    uint8_t count;
    int32_t x[2], y[2];
} ui_touch_t;

typedef enum {
    UI_GESTURE_IDLE = 0,
    UI_GESTURE_ONE_DOWN,     // one finger, still undecided
    UI_GESTURE_SWIPED,       // the swipe was reported; waiting for release
    UI_GESTURE_LONG_PRESSED, // the long press was reported; waiting for release
    UI_GESTURE_TWO_DOWN,     // two fingers
} ui_gesture_phase_t;

typedef struct {
    int32_t width, height;
    ui_gesture_phase_t phase;
    int32_t start_x, start_y;  // where the finger came down, panel coordinates
    int64_t start_ms;
    int32_t farthest;          // the most it has been away from there
    uint8_t fingers_before;
    float last_angle, turned;  // radians
    bool rotate_reported;
} ui_gesture_state_t;

void ui_gesture_init(ui_gesture_state_t *state, int32_t width, int32_t height);

// One sample. `rotation` is how the screen is turned (0, 90, 180, 270).
ui_gesture_t ui_gesture_feed(ui_gesture_state_t *state, const ui_touch_t *touch, int64_t now_ms, int rotation);

// A point of the panel as the viewer sees the screen when it is turned.
void ui_gesture_to_view(int32_t width, int32_t height, int rotation, int32_t *x, int32_t *y);

const char *ui_gesture_name(ui_gesture_kind_t kind);

#ifdef __cplusplus
}
#endif
