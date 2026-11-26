// SPDX-License-Identifier: Apache-2.0
// The update's decisions that need no hardware. See update_policy.h. Pure.

#include "update_policy.h"

#include <stddef.h>

bool update_check_due(const update_clock_t *clock)
{
    if (clock == NULL || !clock->enabled || !clock->net_up || clock->busy || !clock->net_seen) {
        return false;
    }
    if (!clock->checked) {
        return clock->now_s - clock->net_up_s >= UPDATE_FIRST_CHECK_S;
    }
    const uint32_t wait = clock->last_ok ? UPDATE_CHECK_EVERY_S : UPDATE_RETRY_S;
    return clock->now_s - clock->last_s >= wait;
}

update_trial_t update_trial_verdict(const update_trial_facts_t *facts)
{
    if (facts == NULL) {
        return UPDATE_TRIAL_WAIT;
    }
    const bool reachable = facts->http_up && (facts->net_up || !facts->has_credentials);
    if (facts->boot_done && reachable) {
        return UPDATE_TRIAL_GOOD;
    }
    return facts->uptime_s >= UPDATE_TRIAL_LIMIT_S ? UPDATE_TRIAL_BAD : UPDATE_TRIAL_WAIT;
}
