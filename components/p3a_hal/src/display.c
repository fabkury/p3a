#include "p3a_hal/display.h"

#include <inttypes.h>

#include "board.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "p3a_hal.display";

static lv_display_t *s_display;

struct _lv_display_t *p3a_hal_display_get_handle(void)
{
    return s_display;
}

esp_err_t p3a_hal_display_init(void)
{
    if (s_display) {
        return ESP_OK;
    }

    lv_display_t *disp = bsp_display_start();
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "bsp_display_start failed");

    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    bsp_display_unlock();

    ESP_RETURN_ON_ERROR(p3a_hal_display_set_brightness(10), TAG, "initial backlight");

    s_display = disp;
    ESP_LOGI(TAG, "Display initialised (handle=%p)", (void *)s_display);
    return ESP_OK;
}

esp_err_t p3a_hal_display_set_brightness(int percent)
{
    return board_backlight_set_percent(percent);
}

esp_err_t p3a_hal_display_fill_color(uint32_t rgb888)
{
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_ERR_INVALID_STATE, TAG, "display not initialised");

    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_color_t color = lv_color_hex(rgb888);
    lv_obj_set_style_bg_color(screen, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_invalidate(screen);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Filled screen with color 0x%06" PRIx32, (uint32_t)rgb888);
    return ESP_OK;
}

