// SPDX-License-Identifier: Apache-2.0
// What a gesture does, given what is on the screen. See ui_route.h. Pure.

#include "ui_route.h"

ui_action_t ui_route(p3a_screen_t screen, bool paused, const ui_gesture_t *gesture, int32_t view_width)
{
    const ui_gesture_kind_t kind = gesture->kind;
    if (kind == UI_GESTURE_NONE) {
        return UI_ACTION_NONE;
    }
    switch (screen) {
    case P3A_SCREEN_INTRO:
    case P3A_SCREEN_UPDATE:
    case P3A_SCREEN_USB:
    case P3A_SCREEN_FATAL:
        return UI_ACTION_NONE;
    case P3A_SCREEN_INFO:
    case P3A_SCREEN_SETUP:
    case P3A_SCREEN_REGISTRATION:
        return kind == UI_GESTURE_LONG_PRESS ? UI_ACTION_CLOSE_SCREEN : UI_ACTION_NONE;
    default:
        break;
    }
    // The artwork, or a notice in its place.
    if (paused) {
        return kind == UI_GESTURE_LONG_PRESS ? UI_ACTION_RESUME : UI_ACTION_NONE;
    }
    const bool artwork = screen == P3A_SCREEN_ARTWORK;
    switch (kind) {
    case UI_GESTURE_TAP:
        if (!artwork) {
            return UI_ACTION_NONE;
        }
        return gesture->x < view_width / 2 ? UI_ACTION_BACK : UI_ACTION_NEXT;
    case UI_GESTURE_SWIPE_UP:
        return artwork ? UI_ACTION_LIKE : UI_ACTION_NONE;
    case UI_GESTURE_SWIPE_DOWN:
        return artwork ? UI_ACTION_UNLIKE : UI_ACTION_NONE;
    case UI_GESTURE_ROTATE_CW:
        return UI_ACTION_TURN_CW;
    case UI_GESTURE_ROTATE_CCW:
        return UI_ACTION_TURN_CCW;
    case UI_GESTURE_LONG_PRESS:
        return UI_ACTION_OPEN_INFO;
    default:
        return UI_ACTION_NONE;
    }
}

const char *ui_action_name(ui_action_t action)
{
    static const char *const names[] = {"none", "next", "back", "like", "unlike", "turn_cw",
                                        "turn_ccw", "open_info", "close_screen", "resume"};
    return (unsigned)action < sizeof(names) / sizeof(names[0]) ? names[action] : "unknown";
}
