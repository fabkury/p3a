// SPDX-License-Identifier: Apache-2.0
// p3a 2.0 entry point. main owns app_main and the boot sequence, nothing else.
//
// Boot never aborts on a recoverable failure: a step that fails is logged and
// the device carries on with what it has.

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_job.h"
#include "esp_system.h"
#include "p3a_mem.h"
#include "p3a_net.h"
#include "p3a_recovery.h"
#include "p3a_settings.h"
#include "p3a_storage.h"
#include "sdio_bus.h"
#include "slave_ota.h"
#include "sdkconfig.h"

#if CONFIG_P3A_TEST_HOOKS
#include "p3a_test_hooks.h"
#endif

static const char *TAG = "p3a";

// Restarts allowed in a row when the touch controller does not come up.
#define TOUCH_RECOVERY_REBOOTS 1

// The settings as they were at boot. Too large for the main task's stack.
static p3a_settings_t s_settings;

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

    // Settings first: the steps below read them. Also brings up NVS. If it
    // fails the device runs on defaults.
    boot_step("settings", p3a_settings_start());
    p3a_settings_get(&s_settings);

    // The panel comes up painted in the background colour, then the backlight
    // goes to the stored brightness.
    const uint32_t bg = s_settings.background_color;
    boot_step("display", p3a_board_display_init((uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg));
    if (s_settings.brightness != 100) {
        boot_step("brightness", p3a_board_set_brightness(s_settings.brightness));
    }

    // A touch controller that does not answer usually does after a restart,
    // so restart once. If it still does not, play on without touch.
    esp_err_t touch = p3a_board_touch_init(3000);
    boot_step("touch", touch);
    if (p3a_recovery_report(P3A_RECOVERY_TOUCH, touch == ESP_OK, TOUCH_RECOVERY_REBOOTS) == P3A_RECOVERY_REBOOT) {
        ESP_LOGW(TAG, "restarting to retry the touch controller");
        esp_restart();
    }

    boot_step("SDIO bus coordinator", sdio_bus_init());
    // Mount, FAT self-repair, the root folder, the health probe. Without a
    // usable card the device still boots; storage is then latched as failed.
    // An sd_root setting that is no longer acceptable falls back to the default.
    boot_step("storage", p3a_storage_start(s_settings.sd_root));

    // The Wi-Fi co-processor: bring its link up and update its firmware if the
    // image in the slave_fw partition is newer. An update takes over the
    // screen (once the UI exists) and ends in a restart. Nothing to update is
    // the normal case; a missing image is not an error.
    esp_err_t c6 = slave_ota_check_and_update();
    boot_step("co-processor", c6 == ESP_ERR_NOT_FOUND ? ESP_OK : c6);

    // Starts joining Wi-Fi and returns; the rest is reported by events.
    boot_step("network", p3a_net_start());

#if CONFIG_P3A_TEST_HOOKS
    ESP_LOGW(TAG, "TEST BUILD: test hooks are compiled in. Not for release.");
    p3a_test_console_start();
#endif

    ESP_LOGI(TAG, "boot complete");
}
