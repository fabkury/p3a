// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "p3a_board.h"

#if P3A_HAS_BUTTONS

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "board_button";

#define BOOT_BUTTON_GPIO    CONFIG_P3A_BOOT_BUTTON_GPIO
#define DEBOUNCE_MS         50

static TimerHandle_t s_debounce_timer;

/**
 * Debounce timer callback - runs in timer daemon task context (safe to call
 * event_bus_emit_simple here). Re-checks the GPIO level to confirm the button
 * is still pressed (active-low), then emits a toggle-pause event.
 */
static void debounce_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    // Confirm button is still held (active-low: 0 = pressed)
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "BOOT button pressed - toggling pause");
        event_bus_emit_simple(P3A_EVENT_TOGGLE_PAUSE);
    }
}

/**
 * GPIO ISR handler - called on falling edge (button press).
 * Starts/resets the debounce timer from ISR context.
 */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTimerResetFromISR(s_debounce_timer, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t p3a_board_button_init(void)
{
    ESP_LOGI(TAG, "Initializing BOOT button on GPIO%d", BOOT_BUTTON_GPIO);

    // Create one-shot debounce timer
    s_debounce_timer = xTimerCreate(
        "btn_debounce",
        pdMS_TO_TICKS(DEBOUNCE_MS),
        pdFALSE,   // one-shot
        NULL,
        debounce_timer_cb
    );
    if (!s_debounce_timer) {
        ESP_LOGE(TAG, "Failed to create debounce timer");
        return ESP_ERR_NO_MEM;
    }

    // Configure GPIO as input with pull-up, interrupt on falling edge
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Install GPIO ISR service (shared; safe to call multiple times)
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means ISR service already installed - that's fine
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Attach ISR handler for the button GPIO
    ret = gpio_isr_handler_add(BOOT_BUTTON_GPIO, button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ISR handler add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BOOT button initialized (GPIO%d, active-low, debounce=%dms)",
             BOOT_BUTTON_GPIO, DEBOUNCE_MS);
    return ESP_OK;
}

#endif // P3A_HAS_BUTTONS
