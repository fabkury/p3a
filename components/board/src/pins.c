#include "board/pins.h"

static const board_pin_info_t s_pin_table[] = {
    { BOARD_PIN_LCD_BACKLIGHT, GPIO_NUM_26, "LCD_BLK", "Backlight PWM (active high)" },
    { BOARD_PIN_LCD_RESET, GPIO_NUM_27, "LCD_RST", "Panel reset (active low)" },
    { BOARD_PIN_TOUCH_RESET, GPIO_NUM_23, "TP_RST", "GT911 reset (active low)" },
    { BOARD_PIN_TOUCH_INT, GPIO_NUM_NC, "TP_INT", "GT911 interrupt (unused on this variant)" },
    { BOARD_PIN_I2C_SCL, GPIO_NUM_8, "I2C0_SCL", "Shared: touch + ES8311/ES7210" },
    { BOARD_PIN_I2C_SDA, GPIO_NUM_7, "I2C0_SDA", "Shared: touch + ES8311/ES7210" },
    { BOARD_PIN_SDMMC_CLK, GPIO_NUM_43, "SDMMC_CLK", "TF card clock" },
    { BOARD_PIN_SDMMC_CMD, GPIO_NUM_44, "SDMMC_CMD", "TF card command" },
    { BOARD_PIN_SDMMC_D0, GPIO_NUM_39, "SDMMC_D0", "TF card data0" },
    { BOARD_PIN_SDMMC_D1, GPIO_NUM_40, "SDMMC_D1", "TF card data1" },
    { BOARD_PIN_SDMMC_D2, GPIO_NUM_41, "SDMMC_D2", "TF card data2" },
    { BOARD_PIN_SDMMC_D3, GPIO_NUM_42, "SDMMC_D3", "TF card data3" },
    { BOARD_PIN_AUDIO_MCLK, GPIO_NUM_13, "I2S_MCLK", "Codec master clock" },
    { BOARD_PIN_AUDIO_BCLK, GPIO_NUM_12, "I2S_BCLK", "Codec bit clock" },
    { BOARD_PIN_AUDIO_LRCK, GPIO_NUM_10, "I2S_LRCK", "Codec word select" },
    { BOARD_PIN_AUDIO_DOUT, GPIO_NUM_9, "I2S_DOUT", "Codec DAC data" },
    { BOARD_PIN_AUDIO_DIN, GPIO_NUM_11, "I2S_DIN", "Codec ADC data" },
    { BOARD_PIN_AUDIO_PA_EN, GPIO_NUM_53, "PA_EN", "NS4150B enable" },
};

const board_pin_info_t *board_get_pin_table(size_t *count)
{
    if (count) {
        *count = sizeof(s_pin_table) / sizeof(s_pin_table[0]);
    }
    return s_pin_table;
}

const board_pin_info_t *board_get_pin_info(board_pin_id_t id)
{
    if (id < 0 || id >= BOARD_PIN_COUNT) {
        return NULL;
    }
    return &s_pin_table[id];
}

