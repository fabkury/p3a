// SPDX-License-Identifier: Apache-2.0
// Runs a function on a stack that is in internal RAM. See p3a_internal_call.h.

#include "p3a_internal_call.h"

#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_task.h"

static const char *TAG = "internal_call";

#define STACK_BYTES 6144

static SemaphoreHandle_t s_turn;  // one caller at a time
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static p3a_internal_fn s_fn;
static void *s_arg;
static esp_err_t s_result;

static void runner(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_result = s_fn(s_arg);
        xSemaphoreGive(s_done);
    }
}

esp_err_t p3a_internal_call_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_turn = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (s_turn == NULL || s_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    p3a_task_config_t config = P3A_TASK_CONFIG("internal_call", runner, NULL, STACK_BYTES, P3A_TASK_APP);
    // The whole point of this task: its stack must stay reachable while the
    // flash cache is remapped.
    config.internal_stack = true;
    const esp_err_t err = p3a_task_create(&config, &s_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the task could not be created: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t p3a_internal_call(p3a_internal_fn fn, void *arg)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int here = 0;
    if (!esp_ptr_external_ram(&here)) {
        return fn(arg);  // this stack is internal already
    }
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_turn, portMAX_DELAY);
    s_fn = fn;
    s_arg = arg;
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_done, portMAX_DELAY);
    const esp_err_t result = s_result;
    xSemaphoreGive(s_turn);
    return result;
}
