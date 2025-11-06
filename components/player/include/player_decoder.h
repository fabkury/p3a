#pragma once

#include "player_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void decoder_task(void* arg);
esp_err_t start_decoder(const anim_desc_t* desc);
void stop_decoder(void);

#ifdef __cplusplus
}
#endif

