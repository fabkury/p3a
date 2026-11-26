// SPDX-License-Identifier: Apache-2.0
#include "p3a_job.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_err.h"
#include "p3a_task.h"

#define QUEUE_LENGTH 8
// Sized for the deepest job known so far, a firmware install over HTTPS.
#define RUNNER_STACK_BYTES (12 * 1024)

static const char *TAG = "p3a_job";

typedef struct {
    p3a_job_id_t id;
    p3a_job_fn fn;
    void *arg;
} job_item_t;

typedef struct {
    const char *task_name;
    QueueHandle_t queue;
    TaskHandle_t task;
    p3a_job_id_t current;  // the job this runner is executing, guarded by s_lock
} runner_t;

static runner_t s_runners[P3A_JOBS_RUNNER_COUNT] = {
    [P3A_JOBS_IO] = {.task_name = "p3a_jobs_io"},
    [P3A_JOBS_UI] = {.task_name = "p3a_jobs_ui"},
};
static p3a_job_table_t s_table;
static SemaphoreHandle_t s_lock;
static bool s_started;

static void runner_task(void *arg)
{
    runner_t *runner = arg;
    for (;;) {
        job_item_t item;
        if (xQueueReceive(runner->queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        p3a_job_info_t info = {0};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        p3a_job_table_start(&s_table, item.id);
        p3a_job_table_get(&s_table, item.id, &info);
        runner->current = item.id;
        xSemaphoreGive(s_lock);

        ESP_LOGD(TAG, "job %u (%s) starts", (unsigned)item.id, info.name);
        esp_err_t result = item.fn(item.arg);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        runner->current = P3A_JOB_ID_NONE;
        p3a_job_table_finish(&s_table, item.id, result);
        xSemaphoreGive(s_lock);

        if (result == ESP_OK) {
            ESP_LOGD(TAG, "job %u (%s) done", (unsigned)item.id, info.name);
        } else {
            ESP_LOGW(TAG, "job %u (%s) failed: %s", (unsigned)item.id, info.name, p3a_err_to_name(result));
        }
    }
}

esp_err_t p3a_jobs_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    p3a_job_table_init(&s_table);

    for (int i = 0; i < P3A_JOBS_RUNNER_COUNT; i++) {
        runner_t *runner = &s_runners[i];
        runner->queue = xQueueCreate(QUEUE_LENGTH, sizeof(job_item_t));
        if (!runner->queue) {
            return ESP_ERR_NO_MEM;
        }
        p3a_task_config_t task =
            P3A_TASK_CONFIG(runner->task_name, runner_task, runner, RUNNER_STACK_BYTES, P3A_TASK_APP);
        esp_err_t err = p3a_task_create(&task, &runner->task);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t p3a_job_submit(p3a_job_runner_t runner_id, const char *name, p3a_job_fn fn, void *arg, p3a_job_id_t *out_id)
{
    if (!fn || (int)runner_id < 0 || runner_id >= P3A_JOBS_RUNNER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    runner_t *runner = &s_runners[runner_id];

    // Record and enqueue under one lock, so that a job cannot be queued
    // without a record or recorded without being queued.
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    job_item_t item = {.id = p3a_job_table_add(&s_table, name), .fn = fn, .arg = arg};
    if (out_id != NULL) {
        p3a_job_table_watch(&s_table, item.id);  // the caller will be asked about it
    }
    if (item.id == P3A_JOB_ID_NONE) {
        err = ESP_ERR_NO_MEM;
    } else if (xQueueSend(runner->queue, &item, 0) != pdTRUE) {
        p3a_job_table_finish(&s_table, item.id, ESP_ERR_NO_MEM);
        err = ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not queue job '%s' on %s", name ? name : "", runner->task_name);
        return err;
    }
    if (out_id) {
        *out_id = item.id;
    }
    return ESP_OK;
}

bool p3a_job_get(p3a_job_id_t id, p3a_job_info_t *out)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = p3a_job_table_get(&s_table, id, out);
    xSemaphoreGive(s_lock);
    return found;
}

void p3a_job_set_progress(uint8_t percent)
{
    if (!s_started) {
        return;
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < P3A_JOBS_RUNNER_COUNT; i++) {
        if (s_runners[i].task == self) {
            p3a_job_table_progress(&s_table, s_runners[i].current, percent);
            break;
        }
    }
    xSemaphoreGive(s_lock);
}
