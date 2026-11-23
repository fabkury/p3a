// SPDX-License-Identifier: Apache-2.0
// p3a 2.0 entry point. main owns app_main and the boot sequence, nothing else.
//
// Boot never aborts on a recoverable failure: a step that fails is logged and
// the device carries on with what it has.

#include <stdio.h>
#include <stdlib.h>

#include "animation_decoder.h"
#include "animation_player.h"
#include "display_renderer.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "psram_alloc.h"
#include "esp_timer.h"
#include "frame_trace.h"
#include "p3a_board.h"
#include "play_scheduler.h"
#include "source_local.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_job.h"
#include "esp_system.h"
#include "p3a_mem.h"
#include "p3a_net.h"
#include "p3a_recovery.h"
#include "p3a_settings.h"
#include "p3a_storage.h"
#include "p3a_ui.h"
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

// What the information screen says. Gathered here because the network is
// above the UI.
static void fill_info(p3a_ui_content_t *out)
{
    p3a_net_status_t net;
    p3a_net_status(&net);
    const esp_app_desc_t *app = esp_app_get_description();
    const int64_t minutes = esp_timer_get_time() / 60000000;
    char uptime[24], signal[32] = "", card[48] = "Not usable";
    if (minutes >= 1440) {
        snprintf(uptime, sizeof(uptime), "%dd %dh %dm", (int)(minutes / 1440), (int)(minutes % 1440 / 60), (int)(minutes % 60));
    } else if (minutes >= 60) {
        snprintf(uptime, sizeof(uptime), "%dh %dm", (int)(minutes / 60), (int)(minutes % 60));
    } else {
        snprintf(uptime, sizeof(uptime), "%dm", (int)minutes);
    }
    const p3a_storage_space_t space = p3a_storage_space();
    if (space.valid) {
        snprintf(card, sizeof(card), "%.1f GB free", (double)space.free_bytes / 1e9);
    }
    const bool up = net.state == P3A_NET_UP;
    if (up) {
        snprintf(signal, sizeof(signal), "%d dBm (%s)", net.rssi,
                 net.rssi >= -60 ? "good" : net.rssi >= -70 ? "fair" : "weak");
    }
    play_scheduler_status_t *player = psram_calloc(1, sizeof(*player));
    uint32_t playable = 0, channels = 0;
    if (player != NULL) {
        play_scheduler_get_status(player);
        channels = player->channel_count;
        for (uint32_t c = 0; c < channels; c++) {
            playable += player->channels[c].available;
        }
        free(player);
    }
    const char *name = net.hostname[0] ? net.hostname : "p3a";
    snprintf(out->title, sizeof(out->title), "%s", name);
    out->progress = -1;
    snprintf(out->text, sizeof(out->text),
             "Firmware\t%s\nUptime\t%s\nWi-Fi\t%s\nSignal\t%s\nAddress\t%s\nWeb page\thttp://%s.local\n"
             "SD card\t%s\nPlayset\t%u artworks, %u channel%s",
             app->version, uptime, net.ssid[0] ? net.ssid : "Not set up", up ? signal : p3a_net_state_name(net.state),
             up ? net.ip : "None", name, card, (unsigned)playable, (unsigned)channels,
             channels == 1 ? "" : "s");
    snprintf(out->hint, sizeof(out->hint), "Long press to close");
}

// The co-processor update on the screen: it knows nothing of the UI and is
// given these three.
static void on_update_begin(const char *version_from, const char *version_to, void *ctx)
{
    (void)ctx;
    p3a_ui_content_t words;
    p3a_ui_content(&words, "Updating Wi-Fi", 0, "%s to %s. Do not unplug", version_from, version_to);
    p3a_ui_screen_show(P3A_SCREEN_UPDATE, &words);
}

static void on_update_progress(int percent, const char *status, void *ctx)
{
    (void)ctx;
    p3a_ui_content_t words;
    p3a_ui_content(&words, "Updating Wi-Fi", percent, "%s", status ? status : "");
    p3a_ui_screen_show(P3A_SCREEN_UPDATE, &words);
}

static void on_update_end(void *ctx)
{
    (void)ctx;
    p3a_ui_screen_hide(P3A_SCREEN_UPDATE);
}

static esp_err_t toggle_pause_job(void *arg)
{
    (void)arg;
    return play_scheduler_set_paused(!play_scheduler_is_paused());
}

// On the timer task: hand the work to a job.
static void on_boot_button(void *ctx)
{
    (void)ctx;
    p3a_job_submit(P3A_JOBS_UI, "pause button", toggle_pause_job, NULL, NULL);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "p3a %s (IDF %s, built %s %s)", app->version, app->idf_ver, app->date, app->time);
    ESP_LOGI(TAG, "chip rev v%d.%d, %d cores", chip.revision / 100, chip.revision % 100, chip.cores);

    p3a_mem_install_alloc_failure_report();
    frame_trace_init();  // nothing unless the build traces (lab.py build trace)
    boot_step("event bus", p3a_event_bus_start());
    boot_step("job runners", p3a_jobs_start());

    // Settings first: the steps below read them. Also brings up NVS. If it
    // fails the device runs on defaults.
    boot_step("settings", p3a_settings_start());
    p3a_settings_get(&s_settings);

    // Decoders blend transparency onto the background colour and follow it.
    boot_step("decoders", animation_decoder_start());

    // The panel comes up painted in the background colour, then the backlight
    // goes to the stored brightness.
    const uint32_t bg = s_settings.background_color;
    boot_step("display", p3a_board_display_init((uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg));
    if (s_settings.brightness != 100) {
        boot_step("brightness", p3a_board_set_brightness(s_settings.brightness));
    }
    // From here on the pipeline owns the framebuffers. It shows black until
    // something registers to draw.
    boot_step("display pipeline", display_renderer_start());
    boot_step("player", animation_player_start());

    // The UI comes up with the display: its boot animation covers the rest
    // of the boot, and the steps below can already say things on the screen.
    boot_step("ui", p3a_ui_start());
    p3a_ui_hooks_set(p3a_ui_hook_table());
    p3a_ui_set_info_source(fill_info);
    static const slave_ota_ui_t update_ui = {.begin = on_update_begin, .progress = on_update_progress, .end = on_update_end};
    slave_ota_set_ui(&update_ui);

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
    boot_step("local source", source_local_start());
    boot_step("scheduler", play_scheduler_start());

    // The Wi-Fi co-processor: bring its link up and update its firmware if the
    // image in the slave_fw partition is newer. An update takes over the
    // screen (once the UI exists) and ends in a restart. Nothing to update is
    // the normal case; a missing image is not an error.
    esp_err_t c6 = slave_ota_check_and_update();
    boot_step("co-processor", c6 == ESP_ERR_NOT_FOUND ? ESP_OK : c6);

    // The BOOT button pauses and resumes. Watched only from here on: a press
    // during the co-processor update above does nothing.
    boot_step("button", p3a_board_button_init(on_boot_button, NULL));

    // Starts joining Wi-Fi and returns; the rest is reported by events.
    boot_step("network", p3a_net_start());

#if CONFIG_P3A_TEST_HOOKS
    ESP_LOGW(TAG, "TEST BUILD: test hooks are compiled in. Not for release.");
    p3a_test_console_start();
#endif

    ESP_LOGI(TAG, "boot complete");
}
