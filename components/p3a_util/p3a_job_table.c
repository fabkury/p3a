// SPDX-License-Identifier: Apache-2.0
#include "p3a_job_table.h"

#include <string.h>

void p3a_job_table_init(p3a_job_table_t *table)
{
    memset(table, 0, sizeof(*table));
}

static p3a_job_info_t *find(p3a_job_table_t *table, p3a_job_id_t id)
{
    if (id == P3A_JOB_ID_NONE) {
        return NULL;
    }
    for (int i = 0; i < P3A_JOB_TABLE_SIZE; i++) {
        if (table->slots[i].id == id) {
            return &table->slots[i];
        }
    }
    return NULL;
}

p3a_job_id_t p3a_job_table_add(p3a_job_table_t *table, const char *name)
{
    // Prefer a slot never used; otherwise a finished job, one nobody watches
    // before one somebody does, and among equals the lowest id, which is the
    // oldest because ids only grow.
    p3a_job_info_t *slot = NULL;
    for (int i = 0; i < P3A_JOB_TABLE_SIZE; i++) {
        p3a_job_info_t *s = &table->slots[i];
        if (s->id == P3A_JOB_ID_NONE) {
            slot = s;
            break;
        }
        if (s->state != P3A_JOB_DONE) {
            continue;
        }
        if (!slot || (slot->watched && !s->watched) || (slot->watched == s->watched && s->id < slot->id)) {
            slot = s;
        }
    }
    if (!slot) {
        return P3A_JOB_ID_NONE;
    }

    if (++table->last_id == P3A_JOB_ID_NONE) {
        table->last_id = 1;  // wrapped after four billion jobs
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = table->last_id;
    slot->state = P3A_JOB_QUEUED;
    slot->result = ESP_OK;
    if (name) {
        strncpy(slot->name, name, sizeof(slot->name) - 1);
    }
    return slot->id;
}

bool p3a_job_table_watch(p3a_job_table_t *table, p3a_job_id_t id)
{
    p3a_job_info_t *job = find(table, id);
    if (!job) {
        return false;
    }
    job->watched = true;
    return true;
}

bool p3a_job_table_start(p3a_job_table_t *table, p3a_job_id_t id)
{
    p3a_job_info_t *job = find(table, id);
    if (!job || job->state != P3A_JOB_QUEUED) {
        return false;
    }
    job->state = P3A_JOB_RUNNING;
    return true;
}

bool p3a_job_table_progress(p3a_job_table_t *table, p3a_job_id_t id, uint8_t percent)
{
    p3a_job_info_t *job = find(table, id);
    if (!job || job->state != P3A_JOB_RUNNING) {
        return false;
    }
    job->progress = percent > 100 ? 100 : percent;
    return true;
}

bool p3a_job_table_finish(p3a_job_table_t *table, p3a_job_id_t id, esp_err_t result)
{
    p3a_job_info_t *job = find(table, id);
    if (!job || job->state == P3A_JOB_DONE) {
        return false;
    }
    job->state = P3A_JOB_DONE;
    job->result = result;
    if (result == ESP_OK) {
        job->progress = 100;
    }
    return true;
}

bool p3a_job_table_get(const p3a_job_table_t *table, p3a_job_id_t id, p3a_job_info_t *out)
{
    const p3a_job_info_t *job = find((p3a_job_table_t *)table, id);
    if (!job) {
        return false;
    }
    if (out) {
        *out = *job;
    }
    return true;
}

const char *p3a_job_state_name(p3a_job_state_t state)
{
    switch (state) {
    case P3A_JOB_QUEUED:
        return "queued";
    case P3A_JOB_RUNNING:
        return "running";
    case P3A_JOB_DONE:
        return "done";
    }
    return "invalid";
}
