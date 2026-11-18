// SPDX-License-Identifier: Apache-2.0
// Recovery by reboot, counted. Some failures at boot (a touch controller
// that does not answer, a co-processor link that does not come up) usually
// clear on a restart, so the device restarts itself: a bounded number of
// times in a row, after which it carries on without the thing that failed.
//
// Two counters per kind, kept in NVS: the streak (reboots in a row, cleared
// by the first success) bounds the loop, and the total (never cleared short
// of a factory reset) is what the status page reports.
//
// p3a_recovery_policy() is host-testable; the rest needs NVS, which
// p3a_settings_start() brings up.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_RECOVERY_TOUCH,  // the touch controller did not initialise
    P3A_RECOVERY_WIFI,   // Wi-Fi stopped working and did not come back
    P3A_RECOVERY_LINK,   // the link to the co-processor failed
    P3A_RECOVERY_KIND_COUNT_
} p3a_recovery_kind_t;

typedef enum {
    P3A_RECOVERY_CONTINUE,  // it worked
    P3A_RECOVERY_REBOOT,    // it failed: restart and try again (already counted)
    P3A_RECOVERY_DEGRADED,  // it failed again after the allowed restarts: carry on without it
} p3a_recovery_action_t;

// The rule, with no side effects.
static inline p3a_recovery_action_t p3a_recovery_policy(uint16_t streak, bool ok, uint16_t max_reboots)
{
    if (ok) {
        return P3A_RECOVERY_CONTINUE;
    }
    return streak < max_reboots ? P3A_RECOVERY_REBOOT : P3A_RECOVERY_DEGRADED;
}

// Reports how a step went and says what to do next. Success clears the
// streak. A failure answered with REBOOT has been counted; the caller
// restarts the device. If the counters cannot be read or written the answer
// to a failure is DEGRADED, never a restart that could not be bounded.
p3a_recovery_action_t p3a_recovery_report(p3a_recovery_kind_t kind, bool ok, uint16_t max_reboots);

uint16_t p3a_recovery_total(p3a_recovery_kind_t kind);
uint16_t p3a_recovery_streak(p3a_recovery_kind_t kind);

// "touch", "wifi", "link".
const char *p3a_recovery_name(p3a_recovery_kind_t kind);

#ifdef __cplusplus
}
#endif
