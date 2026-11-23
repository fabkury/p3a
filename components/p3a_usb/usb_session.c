// SPDX-License-Identifier: Apache-2.0
// Sharing the SD card with a computer over USB. See usb_session.h. Pure.

#include "usb_session.h"

void usb_session_init(usb_session_t *session)
{
    *session = (usb_session_t){.unmounted_ms = -1, .recheck_ms = -1, .settle_ms = -1, .restart_ms = -1};
}

static usb_command_t begin(usb_session_t *session)
{
    session->unmounted_ms = -1;
    session->recheck_ms = -1;
    session->settle_ms = -1;  // the session came back: no restart yet
    session->active = true;
    if (session->taken) {
        return USB_DO_NOTHING;
    }
    session->taken = true;
    return USB_DO_TAKE_CARD;
}

usb_command_t usb_session_mounted(usb_session_t *session, int64_t now_ms)
{
    if (session->restarting) {
        return USB_DO_NOTHING;
    }
    if (session->unmounted_ms >= 0 && now_ms - session->unmounted_ms < USB_SESSION_BOUNCE_MS) {
        // Probably the cable on its way out. If the computer really is back,
        // the recheck finds it still configured.
        session->recheck_ms = session->unmounted_ms + USB_SESSION_RECHECK_MS;
        return USB_DO_NOTHING;
    }
    return begin(session);
}

static void ended(usb_session_t *session, int64_t now_ms)
{
    session->active = false;
    session->recheck_ms = -1;
    if (session->taken && !session->restarting) {
        session->settle_ms = now_ms + USB_SESSION_SETTLE_MS;
    }
}

void usb_session_unmounted(usb_session_t *session, int64_t now_ms)
{
    session->unmounted_ms = now_ms;
    ended(session, now_ms);
}

void usb_session_suspended(usb_session_t *session, int64_t now_ms)
{
    // Not a disconnect: the time of the last unmount is left alone, or a
    // suspend during plug-in would make the real mount look like a bounce.
    ended(session, now_ms);
}

void usb_session_resumed(usb_session_t *session, bool still_configured)
{
    session->unmounted_ms = -1;
    if (!session->restarting && still_configured && session->taken) {
        session->settle_ms = -1;
        session->active = true;
    }
}

usb_command_t usb_session_tick(usb_session_t *session, int64_t now_ms, bool configured)
{
    if (session->restarting) {
        if (!session->restarted && now_ms >= session->restart_ms) {
            session->restarted = true;
            return USB_DO_RESTART;
        }
        return USB_DO_NOTHING;
    }
    if (session->recheck_ms >= 0 && now_ms >= session->recheck_ms) {
        session->recheck_ms = -1;
        if (configured && !session->active) {
            return begin(session);
        }
    }
    if (session->settle_ms >= 0 && now_ms >= session->settle_ms && !session->active) {
        session->settle_ms = -1;
        session->restarting = true;
        session->restart_ms = now_ms + USB_SESSION_NOTICE_MS;
        return USB_DO_ANNOUNCE_RESTART;
    }
    return USB_DO_NOTHING;
}
