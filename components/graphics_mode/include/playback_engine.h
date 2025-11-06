#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t playback_engine_init(void);
esp_err_t playback_engine_start(const char *path);
esp_err_t playback_engine_switch(const char *path);
esp_err_t playback_engine_stop(void);
bool playback_engine_is_running(void);

#ifdef __cplusplus
}
#endif


