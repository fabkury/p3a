// SPDX-License-Identifier: Apache-2.0
// p3a 2.0 entry point. main owns app_main and the boot sequence, nothing else.

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"

static const char *TAG = "p3a";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "p3a %s (IDF %s, built %s %s)", app->version, app->idf_ver, app->date, app->time);
    ESP_LOGI(TAG, "chip rev v%d.%d, %d cores", chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "boot complete");
}
