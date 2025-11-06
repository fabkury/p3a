#include "p3a_hal/touch.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "p3a_hal.touch";

static lv_indev_t *s_indev;

struct _lv_indev_t *p3a_hal_touch_get_indev(void)
{
    return s_indev;
}

esp_err_t p3a_hal_touch_init(void)
{
    if (s_indev) {
        return ESP_OK;
    }

    lv_indev_t *indev = bsp_display_get_input_dev();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_ERR_INVALID_STATE, TAG, "display indev not ready");

    s_indev = indev;
    ESP_LOGI(TAG, "Touch input initialised");
    return ESP_OK;
}

