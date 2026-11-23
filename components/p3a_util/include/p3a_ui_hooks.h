// SPDX-License-Identifier: Apache-2.0
// What any layer may ask the on-device UI to put on the screen.
//
// The UI sits near the top of the layers, and a few things below it must
// still be able to say "show this": the co-processor update reports its
// progress, storage reports a repaired card. They call the functions here.
// `app_main` hands the UI's own functions to p3a_ui_hooks_set() at boot;
// until then, and in a build without the UI, the calls do nothing.
//
// This is not the way to learn about the system's state: the UI finds out
// by itself that the network is in setup or that nothing can be played (it
// subscribes to the events and asks the scheduler). Use these calls only
// for what nothing but the caller knows.
//
// 1.x reached the UI from below through 27 weak symbols.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The screens, from the one that gives way to every other to the one that
// gives way to none. When several are up, the last listed is seen.
typedef enum {
    P3A_SCREEN_ARTWORK = 0,   // nothing covers the artwork
    P3A_SCREEN_NOTICE,        // a notice about playback or the card; a banner while an artwork plays
    P3A_SCREEN_INTRO,         // the boot animation; put up and taken down by the UI itself
    P3A_SCREEN_INFO,          // the information screen the user opened
    P3A_SCREEN_CARD,          // the SD card needs a person: no usable card, or the format flow
    P3A_SCREEN_REGISTRATION,  // the Makapix registration code
    P3A_SCREEN_SETUP,         // how to put the device on Wi-Fi
    P3A_SCREEN_USB,           // the card is shared with a computer
    P3A_SCREEN_UPDATE,        // a firmware update in progress
    P3A_SCREEN_FATAL,         // the device cannot go on; never taken down
    P3A_SCREEN_COUNT
} p3a_screen_t;

// The notices, from least to most important. When several are up, the last
// listed is seen.
typedef enum {
    P3A_NOTICE_LOADING = 0,     // channels are being listed and nothing can be played yet
    P3A_NOTICE_DOWNLOADING,     // an artwork is on its way (with progress)
    P3A_NOTICE_NOTHING_TO_PLAY, // every channel is listed and there is nothing
    P3A_NOTICE_SOURCE_PROBLEM,  // a source refuses: no key, rate limited, not signed in
    P3A_NOTICE_PLAY_PROBLEM,    // what a person asked for could not be shown
    P3A_NOTICE_CARD_REPAIRED,   // the card's file system was repaired at boot
    P3A_NOTICE_CARD_FAILED,     // the card cannot be used
    P3A_NOTICE_COUNT
} p3a_notice_t;

// The icons the UI can flash in a corner of the artwork, for a few
// seconds, as the answer to a gesture.
typedef enum {
    P3A_ICON_NONE = 0,
    P3A_ICON_LIKE,
    P3A_ICON_UNLIKE,
    P3A_ICON_CLICK,
    P3A_ICON_PIN,
    P3A_ICON_UNPIN,
    P3A_ICON_ERROR,
    P3A_ICON_COUNT
} p3a_icon_t;

// Which corner, as the viewer sees the screen. Reactions go top right and
// pins top left, so that one gesture can answer with both.
typedef enum {
    P3A_ICON_TOP_RIGHT = 0,
    P3A_ICON_TOP_LEFT,
    P3A_ICON_PLACES
} p3a_icon_place_t;

#define P3A_UI_TITLE_LEN 48
#define P3A_UI_TEXT_LEN  400
#define P3A_UI_HINT_LEN  48
#define P3A_UI_BUTTON_LEN 28
#define P3A_UI_BUTTONS    2

// What a screen or a notice says. `text` is lines separated by newlines. A
// line is a paragraph, wrapped and centred, unless it has a tab in it: then
// it is a row, "label<tab>value", and the rows of a screen line up in two
// columns. `hint` is one small line at the bottom of a screen, for what a
// person can do there ("Long press to close"). `buttons` are up to two
// labelled touch targets, stacked above the hint, the first on top; an
// empty label is no button, and one not flagged in `buttons_enabled` is
// drawn dimmed and does nothing.
typedef struct {
    char title[P3A_UI_TITLE_LEN];
    char text[P3A_UI_TEXT_LEN];
    char hint[P3A_UI_HINT_LEN];
    char buttons[P3A_UI_BUTTONS][P3A_UI_BUTTON_LEN];
    uint8_t buttons_enabled;  // bit i: button i takes taps
    int16_t progress;  // 0 to 100, or -1 for none
} p3a_ui_content_t;

typedef struct {
    void (*screen_show)(p3a_screen_t screen, const p3a_ui_content_t *content);
    void (*screen_hide)(p3a_screen_t screen);
    // ttl_ms: 0 stays until hidden. max_per_boot: 0 is no limit.
    void (*notice_show)(p3a_notice_t notice, const p3a_ui_content_t *content, uint32_t ttl_ms, uint32_t max_per_boot);
    void (*notice_hide)(p3a_notice_t notice);
    void (*icon_show)(p3a_icon_t icon, p3a_icon_place_t place);
} p3a_ui_hooks_t;

// Called once by app_main. The table must stay valid for good.
void p3a_ui_hooks_set(const p3a_ui_hooks_t *hooks);

// Puts a screen up, or changes what an up screen says. `content` may be
// NULL for a screen that needs no words. Safe from any task.
void p3a_ui_screen_show(p3a_screen_t screen, const p3a_ui_content_t *content);
void p3a_ui_screen_hide(p3a_screen_t screen);

// Puts a notice up for `ttl_ms` (0: until hidden). With `max_per_boot`, the
// notice is shown at most that many times between restarts and then
// ignored: for conditions that come back every few minutes.
void p3a_ui_notice_show(p3a_notice_t notice, const p3a_ui_content_t *content, uint32_t ttl_ms, uint32_t max_per_boot);
void p3a_ui_notice_hide(p3a_notice_t notice);

// Flashes an icon in a corner for four seconds; a new one in the same
// corner replaces what is there. Safe from any task.
void p3a_ui_icon_show(p3a_icon_t icon, p3a_icon_place_t place);

// Fills a content from a title, a printf-style text and a progress; no hint
// and no buttons.
void p3a_ui_content(p3a_ui_content_t *out, const char *title, int progress, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

#ifdef __cplusplus
}
#endif
