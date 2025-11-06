#pragma once

#include "player_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void renderer_task(void* arg);
void player_renderer_start(void);
void player_renderer_stop(void);

#ifdef __cplusplus
}
#endif

