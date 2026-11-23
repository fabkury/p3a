// SPDX-License-Identifier: Apache-2.0
// What a gesture does, given what is on the screen. Pure: one table as a
// function; host-tested (tests/host/ui/test_ui_route.c). Private to the UI.
//
//   on the artwork (a banner counts as the artwork)
//     tap, right half / left half     next / back
//     swipe up / down                 the artwork's "like" and its undo (wired with the sources, Phase 4)
//     two fingers turned              the screen turns 90 degrees
//     long press                      the information screen
//   paused                            only a long press, which resumes
//   the information screen            long press closes it
//   setup, registration               long press closes them (the device stays in that state)
//   a notice that fills the screen    as on the artwork, except next and back (there is nothing to step through)
//   an update, the card shared over USB, a fatal error
//                                     nothing: these screens are not to be interrupted
//
// 1.x spread this over a router with 27 weak references into other
// components and a "gesture enabled?" question asked from the touch task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "p3a_ui_hooks.h"
#include "ui_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_NEXT,
    UI_ACTION_BACK,
    UI_ACTION_LIKE,       // swipe up
    UI_ACTION_UNLIKE,     // swipe down
    UI_ACTION_TURN_CW,
    UI_ACTION_TURN_CCW,
    UI_ACTION_OPEN_INFO,
    UI_ACTION_CLOSE_SCREEN,  // the screen on top
    UI_ACTION_RESUME,
} ui_action_t;

// `screen` is the screen on top; `view_width` the screen's width as the
// viewer sees it (a tap's x is in the same terms).
ui_action_t ui_route(p3a_screen_t screen, bool paused, const ui_gesture_t *gesture, int32_t view_width);

const char *ui_action_name(ui_action_t action);

#ifdef __cplusplus
}
#endif
