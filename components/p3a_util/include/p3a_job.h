// SPDX-License-Identifier: Apache-2.0
// Job runners: where run-once, possibly slow work goes. A job is a function
// and an argument; it is queued on one of two long-lived runner tasks and
// its outcome stays readable by id for a while afterwards.
//
// Use a job instead of creating a task for one blocking call, and instead of
// doing slow work on a task that must stay responsive (the HTTP server, the
// touch task, an MQTT callback). The HTTP API reports job progress by id.
//
//   P3A_JOBS_IO   storage and network work: a firmware install, a pin write,
//                 a card format, a registration exchange.
//   P3A_JOBS_UI   work that owns the screen for a while: a countdown, a
//                 progress display.
//
// Each runner executes one job at a time, in the order submitted. A job that
// must not wait behind another kind of work belongs on the other runner.
#pragma once

#include "esp_err.h"
#include "p3a_job_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_JOBS_IO,
    P3A_JOBS_UI,
    P3A_JOBS_RUNNER_COUNT
} p3a_job_runner_t;

// The work. Its return value becomes the job's result.
typedef esp_err_t (*p3a_job_fn)(void *arg);

// Starts both runners. Call once, early in app_main. Safe to call again.
esp_err_t p3a_jobs_start(void);

// Queues a job. On success *out_id (if given) identifies it for p3a_job_get().
// The name is for logs and for the API; it is copied.
//
// Fails with ESP_ERR_INVALID_STATE before p3a_jobs_start(), and with
// ESP_ERR_NO_MEM when the runner's queue is full or the record of recent
// jobs holds only live ones. A failed submit never runs fn: the caller still
// owns arg.
esp_err_t p3a_job_submit(p3a_job_runner_t runner, const char *name, p3a_job_fn fn, void *arg, p3a_job_id_t *out_id);

// Reads a job's state. False when the id is unknown: it was never issued, or
// enough newer jobs have finished since that its record was dropped.
bool p3a_job_get(p3a_job_id_t id, p3a_job_info_t *out);

// Called from inside a job function: reports its own progress, 0 to 100.
// Does nothing when called from anywhere else.
void p3a_job_set_progress(uint8_t percent);

#ifdef __cplusplus
}
#endif
