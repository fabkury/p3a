#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pico8_stream_init(void);

void pico8_stream_reset(void);

#ifdef __cplusplus
}
#endif


