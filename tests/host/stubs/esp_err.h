// SPDX-License-Identifier: Apache-2.0
// Host stand-in for ESP-IDF's esp_err.h: the type and the codes p3a logic
// uses, with the IDF's values. Add a code here when a unit under test needs it.
#pragma once

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC      0x109
#define ESP_ERR_INVALID_VERSION  0x10A
#define ESP_ERR_NOT_FINISHED     0x10C
#define ESP_ERR_NOT_ALLOWED      0x10D

static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "UNKNOWN ERROR";
    }
}
