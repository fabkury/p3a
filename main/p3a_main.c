// SPDX-License-Identifier: Apache-2.0
// p3a 2.0 entry point. main owns app_main and the boot sequence, nothing else.
//
// Boot never aborts on a recoverable failure: a step that fails is logged and
// the device carries on with what it has.

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_job.h"
#include "p3a_mem.h"
#include "sdkconfig.h"

#if CONFIG_P3A_TEST_HOOKS
#include "p3a_test_hooks.h"
#endif

static const char *TAG = "p3a";

// Runs one boot step and reports a failure without stopping the boot.
static void boot_step(const char *what, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot: %s failed: %s", what, p3a_err_to_name(err));
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "p3a %s (IDF %s, built %s %s)", app->version, app->idf_ver, app->date, app->time);
    ESP_LOGI(TAG, "chip rev v%d.%d, %d cores", chip.revision / 100, chip.revision % 100, chip.cores);

    p3a_mem_install_alloc_failure_report();
    boot_step("event bus", p3a_event_bus_start());
    boot_step("job runners", p3a_jobs_start());

#if CONFIG_P3A_TEST_HOOKS
    ESP_LOGW(TAG, "TEST BUILD: test hooks are compiled in. Not for release.");
    p3a_test_console_start();
#endif

    ESP_LOGI(TAG, "boot complete");
}
