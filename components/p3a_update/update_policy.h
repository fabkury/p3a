// SPDX-License-Identifier: Apache-2.0
// The update's decisions that need no hardware: when to look for a new
// release, and whether a newly installed firmware has proved itself.
// Pure, host-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- when to look ---------------------------------------------------------------------

#define UPDATE_FIRST_CHECK_S (2u * 60u)        // after the network comes up the first time
#define UPDATE_CHECK_EVERY_S (12u * 60u * 60u) // after a check that got an answer
#define UPDATE_RETRY_S       (60u * 60u)       // after one that did not

typedef struct {
    bool enabled;         // the owner lets the device look by itself
    bool net_up;
    bool busy;            // a check or an install is running, or the card is with a computer
    bool checked;         // a check was attempted since boot
    bool last_ok;         // and the last one got an answer
    uint32_t now_s;       // seconds since boot
    uint32_t net_up_s;    // when the network first came up (valid once net_up was seen)
    bool net_seen;
    uint32_t last_s;      // when the last attempt ended
} update_clock_t;

// True when the device should look now. 1.x looked first after 12 hours,
// so a unit that was switched off at night never looked (question Q3).
bool update_check_due(const update_clock_t *clock);

// --- has a new firmware proved itself ---------------------------------------------------

// A firmware started for the first time after an install is on trial: the
// bootloader goes back to the previous one at the next restart unless this
// one is marked good. It is marked good once it has shown that its owner
// can still reach it, because a firmware that cannot be reached cannot be
// replaced except over USB. If it has not shown that in time, the device
// restarts, which is the way back.
#define UPDATE_TRIAL_LIMIT_S (10u * 60u)

typedef enum {
    UPDATE_TRIAL_WAIT = 0,
    UPDATE_TRIAL_GOOD,
    UPDATE_TRIAL_BAD,
} update_trial_t;

typedef struct {
    uint32_t uptime_s;
    bool boot_done;        // the boot sequence ran to its end
    bool has_credentials;  // a Wi-Fi network is stored
    bool net_up;           // and it was joined, with an address
    bool http_up;          // the web server answers (in setup mode too)
} update_trial_facts_t;

// GOOD: booted, the web server is up, and either the stored network was
//       joined or there is none (then the device is in setup, reachable on
//       its own network).
// BAD:  UPDATE_TRIAL_LIMIT_S passed without that.
update_trial_t update_trial_verdict(const update_trial_facts_t *facts);

#ifdef __cplusplus
}
#endif
