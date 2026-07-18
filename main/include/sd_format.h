// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_format.h
 * @brief User-initiated SD card format (FAT32, on-device, opt-in)
 *
 * p3a CAN format an SD card for its own use but NEVER does so unasked
 * (auto-format-on-mount-fail is deliberately disabled). This module drives
 * the explicit, touch-only, two-step format flow at two entry points:
 *
 * - Case A ("fatal"): the "No Usable SD Card" screen shown when the card
 *   fails to mount at boot. sd_format_fatal_screen_loop() replaces the old
 *   vTaskSuspend(): it initializes touch itself (nothing else is running at
 *   that boot stage) and polls for taps on a "Format card for p3a" button.
 * - Case B ("info"): a "Format SD card" button on the long-press info
 *   screen, visible only while the sd_health latch is tripped. Taps arrive
 *   through the touch router (sd_format_start / sd_format_handle_tap).
 *
 * Both paths share the same warning panel: a countdown gates the confirm
 * button (placed in a different screen region than the initiator), cancel
 * is always active, and a successful format ends in esp_restart().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Format flow phases (rendered by ugfx_ui_draw_sd_format / fatal screen). */
typedef enum {
    SD_FORMAT_IDLE = 0,     ///< Flow not active (fatal screen or info screen showing)
    SD_FORMAT_PROBING,      ///< Case A only: probe mount running (fatal screen notice)
    SD_FORMAT_NO_CARD,      ///< Case A transient notice (~4 s), returns to IDLE
    SD_FORMAT_CARD_OK,      ///< Case A transient notice (~1.5 s), then esp_restart
    SD_FORMAT_WARNING,      ///< Warning panel; countdown gates confirm
    SD_FORMAT_FORMATTING,   ///< Format running; taps ignored
    SD_FORMAT_FAILED,       ///< Case B terminal notice (~6 s) then reboot
    SD_FORMAT_REBOOTING,    ///< "Done - restarting" (~1.5 s) then esp_restart
} sd_format_phase_t;

/** Which surface initiated the flow (changes warning text and execution). */
typedef enum {
    SD_FORMAT_ORIGIN_FATAL = 0,  ///< "No Usable SD Card" fatal screen (Case A)
    SD_FORMAT_ORIGIN_INFO,       ///< Long-press info screen while latched (Case B)
} sd_format_origin_t;

// ----------------------------------------------------------------------------
// Shared UI geometry (720x720; drawn by ugfx_ui, hit-tested here).
// All touch targets are >= 90 px tall.
// ----------------------------------------------------------------------------

#define SDFMT_COUNTDOWN_MS      3000

// Fatal screen initiating button (bottom-center)
#define SDFMT_BTN_FATAL_X       160
#define SDFMT_BTN_FATAL_Y       590
#define SDFMT_BTN_FATAL_W       400
#define SDFMT_BTN_FATAL_H       110

// Warning panel confirm (right side, mid-height - deliberately a different
// screen region from both initiating buttons)
#define SDFMT_BTN_CONFIRM_X     380
#define SDFMT_BTN_CONFIRM_Y     450
#define SDFMT_BTN_CONFIRM_W     300
#define SDFMT_BTN_CONFIRM_H     110

// Warning panel cancel (bottom-left, always active)
#define SDFMT_BTN_CANCEL_X      40
#define SDFMT_BTN_CANCEL_Y      580
#define SDFMT_BTN_CANCEL_W      300
#define SDFMT_BTN_CANCEL_H      110

// Info screen entry button (Case B, only while sd_health latch is tripped)
#define SDFMT_BTN_INFO_X        160
#define SDFMT_BTN_INFO_Y        615
#define SDFMT_BTN_INFO_W        400
#define SDFMT_BTN_INFO_H        90

/**
 * @brief Start the format flow from the info screen (Case B)
 *
 * Called from touch context; non-blocking (the actual format runs on a
 * worker task after the countdown-gated confirm tap).
 *
 * @return ESP_OK on entry into the warning panel;
 *         ESP_ERR_INVALID_STATE if already active, the sd_health latch is
 *         not tripped, or the card is exported over USB.
 */
esp_err_t sd_format_start(sd_format_origin_t origin);

/** Cancel the flow (WARNING phase only; no-op once formatting started). */
void sd_format_cancel(void);

/** True while the flow is anywhere past IDLE (weak-linked from p3a_core). */
bool sd_format_is_active(void);

/**
 * @brief True when the flow owns the whole screen
 *
 * WARNING/FORMATTING/FAILED/REBOOTING render the dedicated panel;
 * PROBING/NO_CARD/CARD_OK render as notices on the fatal screen instead.
 */
bool sd_format_owns_screen(void);

sd_format_phase_t sd_format_get_phase(void);
sd_format_origin_t sd_format_get_origin(void);

/** Remaining ms until the confirm button unlocks; <= 0 means unlocked. */
int sd_format_get_countdown_ms(void);

/**
 * @brief Handle a tap at visual coordinates (weak-linked from p3a_core)
 *
 * Case B: called by the touch router while the flow is active.
 * Case A: called internally by the fatal-screen loop.
 *
 * @return true if the tap was consumed
 */
bool sd_format_handle_tap(uint16_t x, uint16_t y);

/** Long press: cancel in WARNING, no-op elsewhere (weak-linked from p3a_core). */
void sd_format_handle_long_press(void);

/** Case A: true once touch init succeeded and the fatal button should show. */
bool sd_format_fatal_button_ready(void);

/**
 * @brief Case A driver - never returns
 *
 * Replaces vTaskSuspend(NULL) in app_lcd_p4.c's fatal branch. Initializes
 * touch (with a timeout guard; falls back to plain suspend if touch is
 * unavailable), then polls for taps and drives the probe/warning/format
 * state machine. The display renderer must already be started with the
 * fatal-error screen showing.
 *
 * @param fatal_title Title of the fatal screen (string literal; used to
 *                    restore the screen on cancel)
 * @param fatal_body  Body of the fatal screen (string literal)
 */
void sd_format_fatal_screen_loop(const char *fatal_title, const char *fatal_body)
    __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
