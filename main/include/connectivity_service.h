// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include "p3a_state.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t connectivity_service_init(void);
p3a_connectivity_level_t connectivity_service_get_level(void);
esp_err_t connectivity_service_check_ota(void);
esp_err_t connectivity_service_install_ota(void);

#ifdef __cplusplus
}
#endif
