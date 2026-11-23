// SPDX-License-Identifier: Apache-2.0
// The one model of what is on the screen. Pure: no IDF includes, no clock
// of its own (the caller passes the time in milliseconds); host-tested
// (tests/host/ui/test_ui_model.c). Private to the UI.
//
// Nothing "switches to" a screen. Whoever knows a fact says so: the setup
// screen is up, this notice is up, an artwork is playing. What is seen
// follows from all the facts at once, by fixed precedence (the order of
// p3a_screen_t and p3a_notice_t), so that a screen coming down uncovers
// whatever is still up beneath it and two writers cannot fight.
//
// 1.x kept this in seven places (a state machine, a render state with its
// own arbitration among four writers, a UI mode, the display's mode flags
// and three more), and "what is on screen" was whichever wrote last.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "p3a_ui_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

// What is seen, derived from the model.
typedef struct {
    p3a_screen_t screen;  // the screen on top; P3A_SCREEN_ARTWORK when none is
    p3a_notice_t notice;  // which notice, when `screen` is P3A_SCREEN_NOTICE or `banner` is set
    bool banner;          // a notice is shown as a banner over a playing artwork
    bool covers_artwork;  // the artwork is not visible: the UI draws the whole frame
    bool holds_playback;  // the artwork should not move on meanwhile
} ui_view_t;

typedef struct {
    bool up;
    p3a_ui_content_t content;
    int64_t expires_ms;  // 0: never
    uint32_t shows;      // times put up since boot
} ui_slot_t;

typedef struct {
    ui_slot_t screens[P3A_SCREEN_COUNT];  // [P3A_SCREEN_ARTWORK] and [P3A_SCREEN_NOTICE] are not used
    ui_slot_t notices[P3A_NOTICE_COUNT];
    bool playing;       // an artwork is on the panel
    uint32_t revision;  // changes whenever what is seen, or what it says, changes
} ui_model_t;

void ui_model_init(ui_model_t *model);

// Each returns true when what is seen (or what it says) changed, so that the
// caller knows to draw.

// Puts a screen up or changes its words. NULL content keeps the words it
// has. P3A_SCREEN_ARTWORK and P3A_SCREEN_NOTICE cannot be put up this way.
bool ui_model_screen_show(ui_model_t *model, p3a_screen_t screen, const p3a_ui_content_t *content);

// Takes a screen down. P3A_SCREEN_FATAL stays.
bool ui_model_screen_hide(ui_model_t *model, p3a_screen_t screen);

// Puts a notice up. ttl_ms 0 stays until hidden; otherwise it goes by
// itself (ui_model_tick). With max_per_boot, a notice that has been put up
// that many times is ignored; changing the words of one that is up does not
// count as putting it up.
bool ui_model_notice_show(ui_model_t *model, p3a_notice_t notice, const p3a_ui_content_t *content, uint32_t ttl_ms,
                          uint32_t max_per_boot, int64_t now_ms);
bool ui_model_notice_hide(ui_model_t *model, p3a_notice_t notice);

// Whether an artwork is on the panel: decides between a notice as a banner
// and a notice as the whole screen.
bool ui_model_set_playing(ui_model_t *model, bool playing);

// Takes down what has expired.
bool ui_model_tick(ui_model_t *model, int64_t now_ms);

void ui_model_view(const ui_model_t *model, ui_view_t *out);

// The words of what is seen, or NULL when it has none (the artwork alone).
const p3a_ui_content_t *ui_model_content(const ui_model_t *model);

const char *ui_screen_name(p3a_screen_t screen);
const char *ui_notice_name(p3a_notice_t notice);

#ifdef __cplusplus
}
#endif
