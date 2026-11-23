// SPDX-License-Identifier: Apache-2.0
// Fingers on the panel turned into gestures. See ui_gesture.h. Pure.

#include "ui_gesture.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

void ui_gesture_init(ui_gesture_state_t *state, int32_t width, int32_t height)
{
    memset(state, 0, sizeof(*state));
    state->width = width;
    state->height = height;
}

void ui_gesture_to_view(int32_t width, int32_t height, int rotation, int32_t *x, int32_t *y)
{
    const int32_t px = *x, py = *y;
    switch (rotation) {
    case 90:
        *x = py;
        *y = height - 1 - px;
        break;
    case 180:
        *x = width - 1 - px;
        *y = height - 1 - py;
        break;
    case 270:
        *x = width - 1 - py;
        *y = px;
        break;
    default:
        break;
    }
}

static float wrap(float angle)
{
    while (angle > PI_F) {
        angle -= 2.0f * PI_F;
    }
    while (angle < -PI_F) {
        angle += 2.0f * PI_F;
    }
    return angle;
}

static int32_t iabs(int32_t v)
{
    return v < 0 ? -v : v;
}

ui_gesture_t ui_gesture_feed(ui_gesture_state_t *state, const ui_touch_t *touch, int64_t now_ms, int rotation)
{
    ui_gesture_t out = {.kind = UI_GESTURE_NONE};
    const uint8_t fingers = touch->count;
    const uint8_t before = state->fingers_before;
    state->fingers_before = fingers;

    // Two fingers: only turning counts, in the panel's own coordinates (a
    // turn is a turn however the screen is oriented).
    if (fingers >= 2) {
        const float angle = atan2f((float)(touch->y[1] - touch->y[0]), (float)(touch->x[1] - touch->x[0]));
        if (before < 2) {
            state->phase = UI_GESTURE_TWO_DOWN;
            state->last_angle = angle;
            state->turned = 0.0f;
            state->rotate_reported = false;
        } else if (state->phase == UI_GESTURE_TWO_DOWN) {
            state->turned += wrap(angle - state->last_angle);
            state->last_angle = angle;
            if (!state->rotate_reported && fabsf(state->turned) >= (float)UI_GESTURE_ROTATE_DEGREES * PI_F / 180.0f) {
                state->rotate_reported = true;  // once per gesture, however far the fingers go on
                out.kind = state->turned > 0 ? UI_GESTURE_ROTATE_CW : UI_GESTURE_ROTATE_CCW;
            }
        }
        return out;
    }
    if (before >= 2 && state->phase == UI_GESTURE_TWO_DOWN) {
        // One finger lifted, or both. What is left of the touch is not a tap.
        state->phase = fingers == 0 ? UI_GESTURE_IDLE : UI_GESTURE_SWIPED;
        return out;
    }

    const int32_t still = ((state->width < state->height ? state->width : state->height) * UI_GESTURE_STILL_PERMILLE + 500) / 1000;

    if (fingers == 1) {
        if (state->phase == UI_GESTURE_IDLE) {
            state->phase = UI_GESTURE_ONE_DOWN;
            state->start_x = touch->x[0];
            state->start_y = touch->y[0];
            state->start_ms = now_ms;
            state->farthest = 0;
            return out;
        }
        if (state->phase != UI_GESTURE_ONE_DOWN) {
            return out;  // already reported; waiting for the finger to lift
        }
        // Distances as the viewer sees them: "up" is up on the turned screen.
        int32_t x = touch->x[0], y = touch->y[0], x0 = state->start_x, y0 = state->start_y;
        ui_gesture_to_view(state->width, state->height, rotation, &x, &y);
        ui_gesture_to_view(state->width, state->height, rotation, &x0, &y0);
        const int32_t dy = y - y0;
        const int32_t moved = iabs(x - x0) + iabs(dy);
        if (moved > state->farthest) {
            state->farthest = moved;
        }
        if (moved <= still && now_ms - state->start_ms >= UI_GESTURE_LONG_PRESS_MS) {
            state->phase = UI_GESTURE_LONG_PRESSED;
            out.kind = UI_GESTURE_LONG_PRESS;
            return out;
        }
        if (iabs(dy) >= state->height * UI_GESTURE_SWIPE_PERCENT / 100) {
            state->phase = UI_GESTURE_SWIPED;
            out.kind = dy < 0 ? UI_GESTURE_SWIPE_UP : UI_GESTURE_SWIPE_DOWN;
        }
        return out;
    }

    // No finger.
    if (state->phase == UI_GESTURE_ONE_DOWN && state->farthest <= still) {
        out.kind = UI_GESTURE_TAP;
        out.x = state->start_x;
        out.y = state->start_y;
        ui_gesture_to_view(state->width, state->height, rotation, &out.x, &out.y);
    }
    state->phase = UI_GESTURE_IDLE;
    return out;
}

const char *ui_gesture_name(ui_gesture_kind_t kind)
{
    static const char *const names[] = {"none", "tap", "swipe_up", "swipe_down", "long_press", "rotate_cw", "rotate_ccw"};
    return (unsigned)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}
