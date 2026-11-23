// SPDX-License-Identifier: Apache-2.0
// The rules of a turn. See sched_turn.h. Pure.

#include "sched_turn.h"

#include <string.h>

void sched_turn_init(sched_turn_t *turn, uint32_t dwell_s)
{
    memset(turn, 0, sizeof(*turn));
    turn->dwell_s = dwell_s;
}

void sched_turn_begin(sched_turn_t *turn, sched_turn_origin_t origin)
{
    turn->busy = true;
    turn->origin = (uint8_t)origin;
    turn->retries = 0;
    turn->skips = 0;
}

void sched_turn_shown(sched_turn_t *turn, int64_t now_ms)
{
    turn->busy = false;
    turn->timing = true;
    turn->idle = false;
    turn->since_ms = now_ms;
}

sched_turn_verdict_t sched_turn_failed(sched_turn_t *turn, sched_fail_t fail, int64_t now_ms)
{
    sched_turn_verdict_t verdict = {0};
    verdict.set_aside = fail != SCHED_FAIL_OTHER;
    if (fail == SCHED_FAIL_CORRUPT &&
        (!turn->deleted_any || now_ms - turn->deleted_ms >= SCHED_TURN_DELETE_GAP_MS)) {
        // Bounded, so that a card that garbles what it reads cannot empty
        // the store in a burst.
        verdict.delete_blob = true;
        turn->deleted_any = true;
        turn->deleted_ms = now_ms;
    }
    if (!turn->busy) {
        return verdict;  // a failure outside any turn: nothing more to decide
    }
    if (turn->origin != SCHED_TURN_THIS && turn->retries < SCHED_TURN_RETRIES) {
        turn->retries++;
        verdict.retry = true;
        return verdict;
    }
    verdict.report = turn->origin != SCHED_TURN_AUTO;
    sched_turn_gave_up(turn, now_ms);
    return verdict;
}

bool sched_turn_skip(sched_turn_t *turn)
{
    if (!turn->busy || turn->origin == SCHED_TURN_THIS || turn->skips >= SCHED_TURN_SKIPS) {
        return false;
    }
    turn->skips++;
    return true;
}

void sched_turn_gave_up(sched_turn_t *turn, int64_t now_ms)
{
    turn->busy = false;
    turn->idle = !turn->timing;
    turn->since_ms = now_ms;  // what is on the panel, if anything, stays for another dwell
}

void sched_turn_stopped(sched_turn_t *turn)
{
    turn->timing = false;
    turn->idle = false;
}

int32_t sched_turn_remaining_s(const sched_turn_t *turn, int64_t now_ms)
{
    if (turn->paused || turn->holds > 0 || turn->busy || !(turn->timing || turn->idle) || turn->dwell_s == 0) {
        return -1;
    }
    const int64_t left_ms = turn->since_ms + (int64_t)turn->dwell_s * 1000 - now_ms;
    return left_ms <= 0 ? 0 : (int32_t)((left_ms + 999) / 1000);
}

bool sched_turn_due(const sched_turn_t *turn, int64_t now_ms, uint32_t playable)
{
    return playable > (turn->idle ? 0u : 1u) && sched_turn_remaining_s(turn, now_ms) == 0;
}

void sched_turn_set_dwell(sched_turn_t *turn, uint32_t dwell_s)
{
    turn->dwell_s = dwell_s;
}

void sched_turn_set_paused(sched_turn_t *turn, bool paused, int64_t now_ms)
{
    if (turn->paused && !paused) {
        turn->since_ms = now_ms;
    }
    turn->paused = paused;
}

void sched_turn_hold(sched_turn_t *turn)
{
    turn->holds++;
}

void sched_turn_release(sched_turn_t *turn, int64_t now_ms)
{
    if (turn->holds > 0 && --turn->holds == 0) {
        turn->since_ms = now_ms;
    }
}
