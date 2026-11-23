// SPDX-License-Identifier: Apache-2.0
// The SD card as a USB drive: plug the device's USB port into a computer
// and the card appears there, to copy pictures onto.
//
// While a computer has the card the device does not use it at all: playback
// stops, writes are refused, and the screen says so. When the computer lets
// go the device restarts and reads the card afresh. Every session ends that
// way, whatever the computer did (usb_session.h says why).
//
// 1.x's app_usb.c, with the session's rules taken out into pure code.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings USB up. Call once at boot, after storage. Without a card the port
// still comes up and a computer is told there is no medium.
esp_err_t p3a_usb_start(void);

// True from the moment a computer takes the card until the restart: the
// device must not touch the card meanwhile.
bool p3a_usb_is_sharing(void);

// What the bus reports. Called from TinyUSB's callbacks; a test may call it
// to play the computer's part.
typedef enum {
    P3A_USB_MOUNTED = 0,  // the computer configured the device
    P3A_USB_UNMOUNTED,    // it let go, or the cable was pulled
    P3A_USB_SUSPENDED,
    P3A_USB_RESUMED,
} p3a_usb_event_t;
void p3a_usb_session_event(p3a_usb_event_t event);

#ifdef __cplusplus
}
#endif
