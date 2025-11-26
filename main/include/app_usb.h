#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
typedef struct __attribute__((packed)) {
    uint8_t report_id;
    uint8_t flags;
    uint16_t x;  // 0-127
    uint16_t y;  // 0-127
    uint8_t pressure;
    uint8_t reserved;
} pico8_touch_report_t;
#endif

#if CONFIG_P3A_USB_MSC_ENABLE

esp_err_t app_usb_init(void);

bool app_usb_is_stream_active(void);

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
void app_usb_report_touch(const pico8_touch_report_t *report);
#endif

#else

static inline esp_err_t app_usb_init(void)
{
    return ESP_OK;
}

static inline bool app_usb_is_stream_active(void)
{
    return false;
}

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
static inline void app_usb_report_touch(const pico8_touch_report_t *report)
{
    (void)report;
}
#endif

#endif  // CONFIG_P3A_USB_MSC_ENABLE

#ifdef __cplusplus
}
#endif


