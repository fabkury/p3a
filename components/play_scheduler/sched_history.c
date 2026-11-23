// SPDX-License-Identifier: Apache-2.0
#include "sched_history.h"

#include <string.h>

void sched_history_clear(sched_history_t *history)
{
    memset(history, 0, sizeof(*history));
}

// The record `steps` back from the newest.
static const p3a_record_t *at(const sched_history_t *history, uint32_t steps)
{
    return &history->records[(history->newest + SCHED_HISTORY_SIZE - steps) % SCHED_HISTORY_SIZE];
}

void sched_history_add(sched_history_t *history, const p3a_record_t *record)
{
    history->position = 0;
    if (history->count > 0 && p3a_record_same_identity(at(history, 0), record)) {
        return;
    }
    history->newest = history->count == 0 ? 0 : (history->newest + 1) % SCHED_HISTORY_SIZE;
    history->records[history->newest] = *record;
    if (history->count < SCHED_HISTORY_SIZE) {
        history->count++;
    }
}

bool sched_history_back(sched_history_t *history, p3a_record_t *out)
{
    if (history->position + 1 >= history->count) {
        return false;
    }
    history->position++;
    *out = *at(history, history->position);
    return true;
}

bool sched_history_forward(sched_history_t *history, p3a_record_t *out)
{
    if (history->position == 0) {
        return false;
    }
    history->position--;
    *out = *at(history, history->position);
    return true;
}
