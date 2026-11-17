/*
 * Compatibility layer for AnimatedGIF library to work with ESP-IDF
 * Provides Arduino-style functions that ESP-IDF doesn't have
 */

#ifndef ARDUINO_COMPAT_H
#define ARDUINO_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Arduino millis() equivalent - returns milliseconds since boot
static inline unsigned long millis(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

// Arduino delay() equivalent - delays for specified milliseconds
static inline void delay(unsigned long ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#ifdef __cplusplus
}
#endif

#endif // ARDUINO_COMPAT_H



