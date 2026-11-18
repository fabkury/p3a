// SPDX-License-Identifier: Apache-2.0
// Host stand-in for ESP-IDF's esp_log.h: logging compiles to nothing, but the
// arguments are still type-checked.
#pragma once

#include <stdio.h>

#define P3A_HOST_LOG_(tag, ...)     do {                                if (0) {                            (void)(tag);                    printf(__VA_ARGS__);        }                           } while (0)
#define ESP_LOGE(tag, ...) P3A_HOST_LOG_(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) P3A_HOST_LOG_(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) P3A_HOST_LOG_(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) P3A_HOST_LOG_(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) P3A_HOST_LOG_(tag, __VA_ARGS__)
