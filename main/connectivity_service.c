// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "connectivity_service.h"
#include "app_wifi.h"
#include "ota_manager.h"

esp_err_t connectivity_service_init(void)
{
    return app_wifi_init();
}

p3a_connectivity_level_t connectivity_service_get_level(void)
{
    return p3a_state_get_connectivity();
}

esp_err_t connectivity_service_check_ota(void)
{
    return ota_manager_check_for_update();
}

esp_err_t connectivity_service_install_ota(void)
{
    return ota_manager_install_update(NULL, NULL);
}
