// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The GT911 touch controller: bus recovery, bounded initialization, and the
// handle that gesture code reads points from.

#include "p3a_board.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psram_alloc.h"

static const char *TAG = "p3a_board_touch";

static esp_lcd_touch_handle_t s_touch;

// If a slave (the GT911, caught mid-transaction by a reboot) is holding SDA
// low, the I2C peripheral busy-spins forever waiting for the bus. Clock SCL as
// a plain GPIO before the I2C controller takes the pins, to give the slave the
// chance to let go. This is the standard SMBus recovery procedure.
static void i2c_bus_recovery(int scl_gpio, int sda_gpio)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << scl_gpio) | (1ULL << sda_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        return;
    }

    gpio_set_level(scl_gpio, 1);
    gpio_set_level(sda_gpio, 1);
    esp_rom_delay_us(10);

    if (gpio_get_level(sda_gpio) == 1) {
        // The bus is healthy; release the pins for the I2C peripheral.
        gpio_reset_pin(scl_gpio);
        gpio_reset_pin(sda_gpio);
        return;
    }

    ESP_LOGW(TAG, "I2C bus stuck (SDA low on GPIO%d); attempting clock-pulse recovery", sda_gpio);

    int clocks = 0;
    for (clocks = 0; clocks < 16; clocks++) {
        gpio_set_level(scl_gpio, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl_gpio, 1);
        esp_rom_delay_us(5);
        if (gpio_get_level(sda_gpio) == 1) {
            break;
        }
    }

    // STOP condition: SDA low to high while SCL is high.
    gpio_set_level(sda_gpio, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl_gpio, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda_gpio, 1);
    esp_rom_delay_us(5);

    const bool recovered = gpio_get_level(sda_gpio) == 1;
    gpio_reset_pin(scl_gpio);
    gpio_reset_pin(sda_gpio);

    if (recovered) {
        ESP_LOGI(TAG, "I2C bus recovered after %d SCL pulse(s)", clocks + 1);
    } else {
        ESP_LOGE(TAG, "I2C bus recovery failed; SDA still held low after 16 pulses and a STOP");
    }
}

// --- bounded initialization ------------------------------------------------------
//
// Despite the recovery above, bringing up the controller can still hang: the
// I2C driver has paths that spin on a wedged bus. So the blocking call runs on
// a task of its own, and the caller waits for it only so long.
//
// The context is shared by that task and the waiting caller and is freed by
// whichever lets go last. (1.x kept it on the caller's stack, so a timed-out
// caller left the task holding a dead frame: audit item H5.)

typedef struct {
    SemaphoreHandle_t done;
    esp_lcd_touch_handle_t handle;
    esp_err_t result;
    atomic_int refs;
} init_ctx_t;

static void init_ctx_release(init_ctx_t *ctx)
{
    if (atomic_fetch_sub(&ctx->refs, 1) == 1) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }
}

static void init_task(void *arg)
{
    init_ctx_t *ctx = arg;
    i2c_bus_recovery(BSP_I2C_SCL, BSP_I2C_SDA);
    ctx->result = bsp_touch_new(NULL, &ctx->handle);
    xSemaphoreGive(ctx->done);
    init_ctx_release(ctx);
    vTaskDelete(NULL);
}

esp_err_t p3a_board_touch_init(uint32_t timeout_ms)
{
    if (s_touch) {
        return ESP_OK;
    }
    init_ctx_t *ctx = psram_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->result = ESP_FAIL;
    atomic_init(&ctx->refs, 2);  // this function and the task

    // Not p3a_task_create(): that is for tasks that live forever, and this one
    // ends. Not a job either: if it hangs, it must not take a job runner with
    // it. Priority 1, the lowest above idle, so that a hung init spins without
    // starving anything that matters. Internal-RAM stack (the default), so the
    // task can delete itself.
    if (xTaskCreate(init_task, "touch_init", 4096, ctx, 1, NULL) != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result;
    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        result = ctx->result;
        if (result == ESP_OK) {
            s_touch = ctx->handle;
        } else {
            ESP_LOGE(TAG, "touch controller init failed: %s", esp_err_to_name(result));
        }
    } else {
        ESP_LOGE(TAG, "touch controller init did not finish in %u ms (I2C bus wedged?)", (unsigned)timeout_ms);
        result = ESP_ERR_TIMEOUT;
    }
    init_ctx_release(ctx);
    return result;
}

esp_lcd_touch_handle_t p3a_board_get_touch(void)
{
    return s_touch;
}
