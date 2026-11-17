// SPDX-License-Identifier: Apache-2.0
// The record of recent jobs: which are queued, which is running, how the
// finished ones ended. p3a_job.h runs the jobs; this is its bookkeeping,
// kept separate so that it can be tested on the host.
//
// Not thread-safe: the caller serializes access.
// Host-testable: no FreeRTOS or IDF includes beyond esp_err.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_JOB_TABLE_SIZE 16
#define P3A_JOB_NAME_MAX   24  // including the terminator

typedef uint32_t p3a_job_id_t;  // never 0 for a real job
#define P3A_JOB_ID_NONE 0u

typedef enum {
    P3A_JOB_QUEUED,
    P3A_JOB_RUNNING,
    P3A_JOB_DONE,
} p3a_job_state_t;

typedef struct {
    p3a_job_id_t id;
    p3a_job_state_t state;
    esp_err_t result;   // meaningful once DONE
    uint8_t progress;   // 0..100; 100 once DONE with ESP_OK
    char name[P3A_JOB_NAME_MAX];
} p3a_job_info_t;

typedef struct {
    p3a_job_info_t slots[P3A_JOB_TABLE_SIZE];
    p3a_job_id_t last_id;
} p3a_job_table_t;

void p3a_job_table_init(p3a_job_table_t *table);

// Records a new QUEUED job and returns its id. Uses a free slot, or replaces
// the oldest finished job. Returns P3A_JOB_ID_NONE when every slot holds a
// job that is still queued or running. A long name is truncated.
p3a_job_id_t p3a_job_table_add(p3a_job_table_t *table, const char *name);

// State changes. Each returns false, changing nothing, when the id is
// unknown (never issued, or its record was replaced) or the change makes no
// sense: only a QUEUED job can start, only a RUNNING job reports progress,
// and a DONE job stays as it is.
bool p3a_job_table_start(p3a_job_table_t *table, p3a_job_id_t id);
bool p3a_job_table_progress(p3a_job_table_t *table, p3a_job_id_t id, uint8_t percent);
bool p3a_job_table_finish(p3a_job_table_t *table, p3a_job_id_t id, esp_err_t result);

// Copies the record of a job. False when the id is unknown.
bool p3a_job_table_get(const p3a_job_table_t *table, p3a_job_id_t id, p3a_job_info_t *out);

const char *p3a_job_state_name(p3a_job_state_t state);

#ifdef __cplusplus
}
#endif
