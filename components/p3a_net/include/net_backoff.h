// SPDX-License-Identifier: Apache-2.0
// How long to wait before trying to rejoin Wi-Fi after a drop.
//
// The first tries are immediate: most drops are momentary. After that the
// delay doubles from 2 s up to 20 s, which covers a router that takes a
// minute or two to restart without hammering it. With the ten-drop limit
// that precedes a driver re-initialisation, one cycle spans about 97 s.
//
// A device that has never been connected since boot does not back off: it is
// either about to connect or about to fall back to setup.
//
// Host-testable.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_BACKOFF_IMMEDIATE_TRIES 3
#define NET_BACKOFF_FIRST_MS        2000u
#define NET_BACKOFF_MAX_MS          20000u

// `attempt` counts from 1. Returns milliseconds; 0 means "now".
static inline uint32_t net_backoff_delay_ms(int attempt, bool ever_connected)
{
    if (!ever_connected || attempt <= NET_BACKOFF_IMMEDIATE_TRIES) {
        return 0;
    }
    int doublings = attempt - NET_BACKOFF_IMMEDIATE_TRIES - 1;
    if (doublings > 8) {
        return NET_BACKOFF_MAX_MS;
    }
    uint32_t delay = NET_BACKOFF_FIRST_MS << doublings;
    return delay > NET_BACKOFF_MAX_MS ? NET_BACKOFF_MAX_MS : delay;
}

#ifdef __cplusplus
}
#endif
