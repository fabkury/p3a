#include "board.h"

#include "board/pins.h"

#include "bsp/display.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

static const char *TAG = "board";

static bool s_initialized;
static esp_ldo_channel_handle_t s_ldo_dphy;
static esp_ldo_channel_handle_t s_ldo_vo4;

static esp_err_t board_enable_power_domains(void)
{
    if (!s_ldo_dphy) {
        const esp_ldo_channel_config_t cfg = {
            .chan_id = 3,
            .voltage_mv = 2500,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &s_ldo_dphy), TAG, "enable VDD_MIPI_DPHY");
    }

    if (!s_ldo_vo4) {
        const esp_ldo_channel_config_t cfg = {
            .chan_id = 4,
            .voltage_mv = 3300,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &s_ldo_vo4), TAG, "enable LDO VO4");
    }

    return ESP_OK;
}

static esp_err_t board_configure_output(gpio_num_t gpio, int level)
{
    if (gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(gpio_reset_pin(gpio), TAG, "reset pin");
    ESP_RETURN_ON_ERROR(gpio_set_direction(gpio, GPIO_MODE_OUTPUT), TAG, "set dir");
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, level), TAG, "set level");
    return ESP_OK;
}

static void board_log_pin(const board_pin_info_t *info)
{
    if (info->gpio == GPIO_NUM_NC) {
        ESP_LOGI(TAG, "%-14s %-8s %s", info->signal, "NC", info->notes);
    } else {
        ESP_LOGI(TAG, "%-14s GPIO%-4d %s", info->signal, info->gpio, info->notes);
    }
}

esp_err_t board_backlight_set_percent(int percent)
{
    return bsp_display_brightness_set(percent);
}

esp_err_t board_backlight_set_enabled(bool on)
{
    return board_backlight_set_percent(on ? 100 : 0);
}

void board_print_pin_map(void)
{
    size_t count = 0;
    const board_pin_info_t *table = board_get_pin_table(&count);
    ESP_LOGI(TAG, "Pin map (%u entries):", (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        board_log_pin(&table[i]);
    }
}

esp_err_t board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(board_enable_power_domains(), TAG, "power domains");
    ESP_RETURN_ON_ERROR(board_configure_output(GPIO_NUM_27, 1), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(board_configure_output(GPIO_NUM_23, 1), TAG, "Touch reset");
    ESP_RETURN_ON_ERROR(board_configure_output(GPIO_NUM_53, 0), TAG, "PA enable");
    ESP_RETURN_ON_ERROR(board_configure_output(GPIO_NUM_26, 0), TAG, "Backlight gate");

    board_print_pin_map();

    s_initialized = true;
    return ESP_OK;
}
