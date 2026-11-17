// SPDX-License-Identifier: Apache-2.0
#include "p3a_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

static const char *TAG = "p3a_task";

esp_err_t p3a_task_create(const p3a_task_config_t *config, TaskHandle_t *out_handle)
{
    if (!config || !config->fn || !config->name || config->stack_bytes == 0 ||
        (int)config->cls < 0 || config->cls >= P3A_TASK_CLASS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->core != P3A_TASK_CORE_CLASS && config->core != P3A_TASK_CORE_ANY &&
        (config->core < 0 || config->core >= portNUM_PROCESSORS)) {
        return ESP_ERR_INVALID_ARG;
    }

    const p3a_task_policy_t policy = p3a_task_policy(config->cls);
    const UBaseType_t priority = p3a_task_priority(config->cls, config->priority_offset);
    const int8_t core = config->core == P3A_TASK_CORE_CLASS ? policy.core : config->core;
    const BaseType_t affinity = core == P3A_TASK_CORE_ANY ? tskNO_AFFINITY : core;
    const bool want_psram = policy.psram_stack && !config->internal_stack;

    TaskHandle_t handle = NULL;
    BaseType_t created = pdFAIL;
    bool on_psram = false;

    if (want_psram) {
        created = xTaskCreatePinnedToCoreWithCaps(config->fn, config->name, config->stack_bytes, config->arg, priority,
                                                  &handle, affinity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        on_psram = created == pdPASS;
        if (!on_psram) {
            ESP_LOGW(TAG, "%s: no PSRAM for a %u-byte stack, using internal RAM", config->name,
                     (unsigned)config->stack_bytes);
        }
    }
    if (created != pdPASS) {
        created = xTaskCreatePinnedToCore(config->fn, config->name, config->stack_bytes, config->arg, priority, &handle,
                                          affinity);
    }
    if (created != pdPASS) {
        ESP_LOGE(TAG, "%s: could not create the task (%u-byte stack)", config->name, (unsigned)config->stack_bytes);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "%s: class %s, priority %u, core %d, %s stack of %u bytes", config->name,
             p3a_task_class_name(config->cls), (unsigned)priority, (int)core, on_psram ? "PSRAM" : "internal",
             (unsigned)config->stack_bytes);
    if (out_handle) {
        *out_handle = handle;
    }
    return ESP_OK;
}
