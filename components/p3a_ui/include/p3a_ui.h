// SPDX-License-Identifier: Apache-2.0
// The on-device UI: what is on the screen besides the artwork.
//
// One model holds every screen and notice that is up, and what is seen
// follows from it (ui_model.h). The UI learns most things by itself: it
// subscribes to the events of the network and the card and asks the
// scheduler about playback. The few callers that must tell it something
// use p3a_ui_hooks.h, from any layer.
//
// A screen is drawn as the display pipeline's UI source and takes the whole
// panel; a notice over a playing artwork is drawn as a band on top of it.
// Text is a built-in pixel font at whole multiples of its size (ui_font.h).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_ui_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the UI. Call once at boot, after the scheduler. app_main then
// hands p3a_ui_hook_table() to p3a_ui_hooks_set().
esp_err_t p3a_ui_start(void);

// The UI's own functions behind p3a_ui_hooks.h.
const p3a_ui_hooks_t *p3a_ui_hook_table(void);

typedef struct {
    p3a_screen_t screen;      // the screen on top; P3A_SCREEN_ARTWORK when none is
    p3a_notice_t notice;      // which notice, when `screen` is P3A_SCREEN_NOTICE or `banner` is set
    bool banner;              // a notice is shown as a banner over the playing artwork
    bool covers_artwork;
    bool has_content;
    p3a_ui_content_t content; // the words of what is seen, when `has_content`
    uint32_t revision;        // changes whenever any of the above does
} p3a_ui_state_t;

void p3a_ui_get_state(p3a_ui_state_t *out);

// --- touch ----------------------------------------------------------------------

// What the information screen says (the device's name, its address, the
// firmware): filled by app_main's function when the screen is opened and
// every two seconds while it is up, because most of it is known only above
// the UI. Called on a job.
typedef void (*p3a_ui_info_fn)(p3a_ui_content_t *out);
void p3a_ui_set_info_source(p3a_ui_info_fn fill);

// Fingers for a test: each step is what the touch controller would report,
// held for `hold_ms`. The touch task reads the steps in place of the
// controller, one sample every 20 ms, so a script takes its real time.
// ESP_ERR_INVALID_STATE while an earlier script is still running.
typedef struct {
    uint8_t fingers;   // 0, 1 or 2
    int32_t x[2], y[2];  // the panel's own coordinates
    uint32_t hold_ms;
} p3a_ui_touch_step_t;
esp_err_t p3a_ui_touch_script(const p3a_ui_touch_step_t *steps, size_t count);

typedef struct {
    bool scripted;             // a script is running
    uint32_t gestures;         // recognised since boot
    const char *last_gesture;  // "tap", "swipe_up", ...; "none" before the first
    const char *last_action;   // what it did: "next", "open_info", "none", ...
} p3a_ui_touch_status_t;
void p3a_ui_touch_get_status(p3a_ui_touch_status_t *out);

const char *p3a_ui_screen_name(p3a_screen_t screen);
const char *p3a_ui_notice_name(p3a_notice_t notice);

#ifdef __cplusplus
}
#endif
