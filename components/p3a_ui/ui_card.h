// SPDX-License-Identifier: Apache-2.0
// Formatting the SD card on the device: the steps a person is taken
// through, as a state machine. Pure: it decides and words; it touches no
// card. Host-tested (tests/host/ui/test_ui_card.c). Private to the UI.
//
// p3a never formats a card unasked. It offers to in two places: at boot,
// when there is no card it can use, and on the information screen, when
// the card that was in use has stopped accepting writes. Either way:
//
//   OFFER     "No usable SD card", with a button                  (boot only)
//   CHECKING  the slot is looked at again                          (boot only)
//     no card answers     -> a notice for 4 s, back to OFFER
//     a good card is in   -> a notice for 1.5 s, restart to use it
//   WARNING   "Everything will be deleted". The confirm button is dead for
//             the first 3 s; cancel (a button, or a long press) always works
//   FORMATTING
//     done    -> a notice for 1.5 s, restart
//     failed  -> a notice for 6 s, restart (the card is in no known state)
//
// Each input answers with what the caller must now do. The words on the
// screen come from ui_card_content().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "p3a_ui_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CARD_CONFIRM_AFTER_MS 3000
#define UI_CARD_NO_CARD_MS       4000
#define UI_CARD_GOOD_MS          1500
#define UI_CARD_DONE_MS          1500
#define UI_CARD_FAILED_MS        6000

typedef enum {
    UI_CARD_NONE = 0,  // the flow is not on the screen
    UI_CARD_OFFER,
    UI_CARD_CHECKING,
    UI_CARD_NO_CARD,
    UI_CARD_GOOD,
    UI_CARD_WARNING,
    UI_CARD_FORMATTING,
    UI_CARD_DONE,
    UI_CARD_FAILED,
} ui_card_phase_t;

typedef enum {
    UI_CARD_DO_NOTHING = 0,
    UI_CARD_DO_PROBE,    // look at the slot; answer with ui_card_probed()
    UI_CARD_DO_FORMAT,   // stop playback and format; answer with ui_card_formatted()
    UI_CARD_DO_RESTART,  // restart the device
} ui_card_command_t;

// What a look at the slot found.
typedef enum {
    UI_CARD_FOUND_NOTHING = 0,  // no card answers
    UI_CARD_FOUND_UNUSABLE,     // a card with no file system p3a can use, or one that failed in use
    UI_CARD_FOUND_GOOD,         // a card that mounts
} ui_card_found_t;

typedef struct {
    ui_card_phase_t phase;
    bool from_boot;        // the flow began as the boot's offer, and cancelling returns to it
    int64_t deadline_ms;   // when the confirm button comes alive, or a notice ends
} ui_card_t;

void ui_card_init(ui_card_t *card);

// The boot found no card it can use: put the offer up.
void ui_card_offer(ui_card_t *card);

// The button on the information screen: straight to the warning. Does
// nothing if the flow is already under way.
void ui_card_ask(ui_card_t *card, int64_t now_ms);

// A tap on one of the screen's buttons (0 is the upper one).
ui_card_command_t ui_card_button(ui_card_t *card, int button, int64_t now_ms);

// A long press: cancels the warning, and does nothing anywhere else.
void ui_card_long_press(ui_card_t *card);

ui_card_command_t ui_card_probed(ui_card_t *card, ui_card_found_t found, int64_t now_ms);
void ui_card_formatted(ui_card_t *card, bool ok, int64_t now_ms);

// Time passing: ends notices. Call a few times a second.
ui_card_command_t ui_card_tick(ui_card_t *card, int64_t now_ms);

// The screen's words and buttons now. False when the flow is not on the
// screen (the screen should be down).
bool ui_card_content(const ui_card_t *card, int64_t now_ms, p3a_ui_content_t *out);

#ifdef __cplusplus
}
#endif
