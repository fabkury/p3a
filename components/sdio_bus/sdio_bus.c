// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// SDIO bus coordinator. See sdio_bus.h.

#include "sdio_bus.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sdio_bus";

static SemaphoreHandle_t s_bus_mutex;
// Who holds the bus, or NULL. Written only by the holder; read by anyone.
static _Atomic(const char *) s_holder;

esp_err_t sdio_bus_init(void)
{
    if (s_bus_mutex) {
        return ESP_OK;
    }
    s_bus_mutex = xSemaphoreCreateMutex();
    return s_bus_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sdio_bus_acquire(uint32_t timeout_ms, const char *requester)
{
    if (!s_bus_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *tag = requester ? requester : "unknown";
    const TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_bus_mutex, ticks) != pdTRUE) {
        const char *holder = atomic_load(&s_holder);
        ESP_LOGW(TAG, "[%s] no SDIO bus after %lu ms (held by %s)", tag, (unsigned long)timeout_ms,
                 holder ? holder : "unknown");
        return ESP_ERR_TIMEOUT;
    }
    atomic_store(&s_holder, tag);
    ESP_LOGD(TAG, "[%s] acquired", tag);
    return ESP_OK;
}

void sdio_bus_release(void)
{
    if (!s_bus_mutex) {
        return;
    }
    ESP_LOGD(TAG, "[%s] released", atomic_load(&s_holder) ? atomic_load(&s_holder) : "unknown");
    atomic_store(&s_holder, NULL);
    xSemaphoreGive(s_bus_mutex);
}

bool sdio_bus_is_locked(void)
{
    return atomic_load(&s_holder) != NULL;
}

const char *sdio_bus_get_holder(void)
{
    return atomic_load(&s_holder);
}
