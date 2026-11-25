// SPDX-License-Identifier: Apache-2.0
// What the Makapix source's files share. Private to the component.
#pragma once

#include "esp_err.h"
#include "source_makapix.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the one long-lived task (makapix_register.c). Call once at boot.
esp_err_t makapix_task_start(void);

// Fills the registration's part of the status: phase, code, time left, last result.
void makapix_register_status(source_makapix_status_t *out);

#ifdef __cplusplus
}
#endif
