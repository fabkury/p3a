// SPDX-License-Identifier: Apache-2.0
// What was shown, for "back" and "forward again". Pure (host-tested).
// Private to the scheduler.
//
// The history holds whole records, so it stays good when the playset
// changes or a channel is listed again: a record says by itself where its
// bytes are. Only artworks that really reached the panel are added (the
// scheduler adds on the player's "shown" report), so a file that failed to
// load never has to be stepped over. 1.x added at pick time.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_HISTORY_SIZE 32

typedef struct {
    p3a_record_t records[SCHED_HISTORY_SIZE];
    uint32_t count;     // how many are held
    uint32_t newest;    // slot of the most recently added
    uint32_t position;  // steps back from the newest: 0 while playing forward
} sched_history_t;

void sched_history_clear(sched_history_t *history);

// Adds an artwork that has just been shown as a new pick, and returns the
// position to the present. Showing the newest one again adds nothing.
void sched_history_add(sched_history_t *history, const p3a_record_t *record);

// Steps one back, to the artwork shown before the one the position is on.
// False at the far end (or with fewer than two held): nothing changes.
bool sched_history_back(sched_history_t *history, p3a_record_t *out);

// Steps one forward again. False when the position is already at the
// present: the caller picks something new instead.
bool sched_history_forward(sched_history_t *history, p3a_record_t *out);

#ifdef __cplusplus
}
#endif
