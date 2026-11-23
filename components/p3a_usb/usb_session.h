// SPDX-License-Identifier: Apache-2.0
// Sharing the SD card with a computer over USB: when a session begins,
// when it is over, and that it always ends in a restart. Pure: the caller
// passes the bus's events and the time; host-tested
// (tests/host/ui/test_usb_session.c). Private to p3a_usb.
//
// The rules, each of them learned on 1.x:
//
// - A session begins when the computer configures the device. From then on
//   the card is the computer's: the device must not read or write it again,
//   because its own view of the file system goes stale the moment the
//   computer writes anything (and computers write to every volume they
//   mount, even when the person touches nothing).
// - So every session ends in a restart, into a fresh mount. There is no
//   "did the computer change anything?" test; it was tried and abandoned.
// - The bus bounces. Pulling the cable can show "configured" again for a few
//   hundred milliseconds; a mount within 1.5 s of an unmount is not taken at
//   once, and is looked at again once the window has passed. A session that
//   ended is given 2 s to come back (a bounce, a short suspend) before the
//   restart is announced.
// - A suspend is not a disconnect, but one that lasts (a laptop put to
//   sleep with the cable in) ends the session all the same.
// - Once the restart is announced nothing calls it off.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_SESSION_BOUNCE_MS   1500  // a mount this soon after an unmount is suspect
#define USB_SESSION_RECHECK_MS  1700  // when a suspect mount is looked at again
#define USB_SESSION_SETTLE_MS   2000  // how long an ended session may come back
#define USB_SESSION_NOTICE_MS   4000  // how long the restart is announced

typedef enum {
    USB_DO_NOTHING = 0,
    USB_DO_TAKE_CARD,         // a session began: stop using the card for good and say so on the screen
    USB_DO_ANNOUNCE_RESTART,  // the session is over: say that the device restarts
    USB_DO_RESTART,
} usb_command_t;

typedef struct {
    bool taken;       // a session has begun at some point: the card is no longer the device's
    bool active;      // the computer may read and write now
    bool restarting;  // the restart is announced
    bool restarted;   // ...and asked for
    int64_t unmounted_ms, recheck_ms, settle_ms, restart_ms;  // -1 when not set
} usb_session_t;

void usb_session_init(usb_session_t *session);

// The computer configured the device.
usb_command_t usb_session_mounted(usb_session_t *session, int64_t now_ms);

// The computer let go, or the cable was pulled.
void usb_session_unmounted(usb_session_t *session, int64_t now_ms);

void usb_session_suspended(usb_session_t *session, int64_t now_ms);
void usb_session_resumed(usb_session_t *session, bool still_configured);

// Time passing. `configured` is whether the computer has the device
// configured right now. Call a few times a second.
usb_command_t usb_session_tick(usb_session_t *session, int64_t now_ms, bool configured);

#ifdef __cplusplus
}
#endif
