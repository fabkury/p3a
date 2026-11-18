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

// --- the first connection after boot --------------------------------------------
//
// Until the device has connected once since boot, a failure may mean that its
// stored network is no use here, and the device should offer setup instead.
// Two cases are told apart by what the radio reports:
//
// - The network is not there. After a power cut the device boots faster than
//   the router, so this is given time: NET_ABSENT_WINDOW_MS, with the same
//   growing delay between tries as a reconnection.
// - The network is there and refuses us (a wrong password, mostly). That will
//   not get better by waiting: NET_INITIAL_TRIES refusals and it is setup.
//   Someone who has just mistyped a password is not kept waiting.

#define NET_INITIAL_TRIES    3
#define NET_ABSENT_WINDOW_MS 120000u

// True when the device should stop trying and offer setup. `refusals` counts
// failures where the network was there; `since_start_ms` is the time since
// joining began.
static inline bool net_first_join_gives_up(int refusals, uint32_t since_start_ms, bool network_absent)
{
    if (refusals >= NET_INITIAL_TRIES) {
        return true;
    }
    return network_absent && since_start_ms >= NET_ABSENT_WINDOW_MS;
}

#ifdef __cplusplus
}
#endif
