#pragma once

#include <stddef.h>

#include "driver/gpio.h"

typedef enum {
    BOARD_PIN_LCD_BACKLIGHT,
    BOARD_PIN_LCD_RESET,
    BOARD_PIN_TOUCH_RESET,
    BOARD_PIN_TOUCH_INT,
    BOARD_PIN_I2C_SCL,
    BOARD_PIN_I2C_SDA,
    BOARD_PIN_SDMMC_CLK,
    BOARD_PIN_SDMMC_CMD,
    BOARD_PIN_SDMMC_D0,
    BOARD_PIN_SDMMC_D1,
    BOARD_PIN_SDMMC_D2,
    BOARD_PIN_SDMMC_D3,
    BOARD_PIN_AUDIO_MCLK,
    BOARD_PIN_AUDIO_BCLK,
    BOARD_PIN_AUDIO_LRCK,
    BOARD_PIN_AUDIO_DOUT,
    BOARD_PIN_AUDIO_DIN,
    BOARD_PIN_AUDIO_PA_EN,
    BOARD_PIN_COUNT
} board_pin_id_t;

typedef struct {
    board_pin_id_t id;
    gpio_num_t gpio;
    const char *signal;
    const char *notes;
} board_pin_info_t;

const board_pin_info_t *board_get_pin_table(size_t *count);
const board_pin_info_t *board_get_pin_info(board_pin_id_t id);

