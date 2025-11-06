#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void graphics_mode_init(void);

void graphics_mode_handle_short_tap(void);
void graphics_mode_handle_long_tap(void);

#ifdef __cplusplus
}
#endif


