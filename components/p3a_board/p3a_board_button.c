// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The BOOT button: interrupt on the falling edge, 50 ms debounce, then one
// callback per press.

#include "p3a_board.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "p3a_board_button";

#define BOOT_BUTTON_GPIO 35
#define DEBOUNCE_MS      50

static TimerHandle_t s_debounce_timer;
static p3a_board_button_cb_t s_on_press;
static void *s_ctx;

// Timer task context. The press counts only if the button is still down after
// the debounce interval (active low: 0 is pressed).
static void debounce_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && s_on_press) {
        s_on_press(s_ctx);
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xTimerResetFromISR(s_debounce_timer, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t p3a_board_button_init(p3a_board_button_cb_t on_press, void *ctx)
{
    if (!on_press) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_debounce_timer) {
        return ESP_ERR_INVALID_STATE;
    }
    s_on_press = on_press;
    s_ctx = ctx;

    s_debounce_timer = xTimerCreate("btn_debounce", pdMS_TO_TICKS(DEBOUNCE_MS), pdFALSE, NULL, debounce_timer_cb);
    if (!s_debounce_timer) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }
    // The ISR service is shared; ESP_ERR_INVALID_STATE means someone installed it already.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_isr_handler_add(BOOT_BUTTON_GPIO, button_isr_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISR handler add failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BOOT button on GPIO%d", BOOT_BUTTON_GPIO);
    return ESP_OK;
}
