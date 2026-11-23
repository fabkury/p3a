// SPDX-License-Identifier: Apache-2.0
// When a channel is listed again. See sched_refresh.h. Pure.

#include "sched_refresh.h"

#include <string.h>

#define NOT_BY_ITSELF INT64_MAX

void sched_refresh_init(sched_refresh_t *r, uint32_t interval_s, bool needs_network)
{
    memset(r, 0, sizeof(*r));
    r->interval_s = interval_s;
    r->needs_network = needs_network;
    r->due_ms = 0;  // never listed: due at once
}

void sched_refresh_known(sched_refresh_t *r, int64_t age_s, int64_t now_ms)
{
    r->ever_listed = true;
    if (r->interval_s == 0) {
        r->due_ms = NOT_BY_ITSELF;
    } else if (age_s < 0 || age_s >= (int64_t)r->interval_s) {
        r->due_ms = now_ms;
    } else {
        r->due_ms = now_ms + ((int64_t)r->interval_s - age_s) * 1000;
    }
}

void sched_refresh_ask(sched_refresh_t *r, int64_t now_ms)
{
    if (r->state == SCHED_REFRESH_RUNNING) {
        return;
    }
    if (r->state == SCHED_REFRESH_STOPPED && r->last == SCHED_LIST_CARD) {
        return;  // nothing brings a failed card back but a restart
    }
    r->state = SCHED_REFRESH_IDLE;
    r->due_ms = now_ms;
}

bool sched_refresh_due(const sched_refresh_t *r, int64_t now_ms, bool network_up)
{
    return r->state == SCHED_REFRESH_IDLE && now_ms >= r->due_ms && (network_up || !r->needs_network);
}

void sched_refresh_started(sched_refresh_t *r)
{
    r->state = SCHED_REFRESH_RUNNING;
}

void sched_refresh_finished(sched_refresh_t *r, sched_list_result_t result, uint32_t retry_after_s, int64_t now_ms)
{
    r->state = SCHED_REFRESH_IDLE;
    r->last = (uint8_t)result;
    int64_t wait_s;
    switch (result) {
    case SCHED_LISTED:
        r->failures = 0;
        r->ever_listed = true;
        r->due_ms = r->interval_s == 0 ? NOT_BY_ITSELF : now_ms + (int64_t)r->interval_s * 1000;
        return;
    case SCHED_LIST_CARD:
        r->state = SCHED_REFRESH_STOPPED;
        r->due_ms = NOT_BY_ITSELF;
        return;
    case SCHED_LIST_RATE_LIMITED:
        wait_s = retry_after_s > SCHED_REFRESH_RATE_LIMIT_MIN_S ? retry_after_s : SCHED_REFRESH_RATE_LIMIT_MIN_S;
        break;
    case SCHED_LIST_NO_KEY:
        wait_s = SCHED_REFRESH_KEY_WAIT_S;
        break;
    default: {
        // One minute, doubling with each failure in a row, up to an hour.
        const uint32_t doublings = r->failures < 6 ? r->failures : 6;
        wait_s = (int64_t)SCHED_REFRESH_RETRY_MIN_S << doublings;
        if (wait_s > SCHED_REFRESH_RETRY_MAX_S) {
            wait_s = SCHED_REFRESH_RETRY_MAX_S;
        }
        break;
    }
    }
    r->failures++;
    r->due_ms = now_ms + wait_s * 1000;
}

int64_t sched_refresh_wait_s(const sched_refresh_t *r, int64_t now_ms)
{
    if (r->state != SCHED_REFRESH_IDLE || r->due_ms == NOT_BY_ITSELF) {
        return r->state == SCHED_REFRESH_RUNNING ? 0 : -1;
    }
    return now_ms >= r->due_ms ? 0 : (r->due_ms - now_ms + 999) / 1000;
}
