// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_format.c
 * @brief User-initiated SD card format (FAT32, on-device, opt-in)
 *
 * See sd_format.h for the flow overview. Execution contexts:
 * - Case A (fatal screen): sd_format_fatal_screen_loop() runs on the main
 *   task, which would otherwise be suspended. It owns touch (nothing else
 *   is initialized at that boot stage) and performs probe/format inline;
 *   the display producer task keeps rendering from this module's state.
 * - Case B (info screen): taps arrive on the touch task via the router;
 *   the format itself runs on a short-lived worker task.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lcd_touch.h"
#include "sdkconfig.h"

#include "bsp/esp-bsp.h"          // bsp_sdcard_mount/unmount, BSP_SD_MOUNT_POINT, bsp_sdcard
#include "esp_vfs_fat.h"          // esp_vfs_fat_sdmmc_mount, esp_vfs_fat_sdcard_format
#include "driver/sdmmc_host.h"    // SDMMC_HOST_DEFAULT, slot config
#include "ff.h"                   // FF_USE_LABEL, f_setlabel
#include "diskio_sdmmc.h"         // ff_diskio_get_pdrv_card

#include "p3a_board.h"
#include "sd_health.h"
#include "sd_format.h"
#include "animation_player.h"     // begin_sd_export, is_sd_export_locked, app_get_screen_rotation
#include "display_renderer.h"     // display_renderer_get_rotation
#include "app_touch.h"            // app_touch_transform_coordinates
#include "ugfx_ui.h"              // ugfx_ui_show_fatal_error

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "sd_format";

// Fatal-screen body shown once the format button is armed (replaces the
// neutral no-touch body passed in by app_lcd_p4.c): the "two paths" text —
// the user self-selects between formatting the inserted card and replacing
// it. Only ever shown together with the button.
#define SDFMT_FATAL_BODY_WITH_BUTTON \
    "A working microSD card is required.\n\n" \
    "Card inserted? Tap below to have\np3a erase and format it.\n\n" \
    "No card? Power off, unscrew the\nback plate, and insert one."

// Notice durations (see sd_format_phase_t)
#define SDFMT_NO_CARD_NOTICE_MS   4000
#define SDFMT_CARD_OK_NOTICE_MS   1500
#define SDFMT_REBOOT_NOTICE_MS    1500
#define SDFMT_FAILED_NOTICE_MS    6000

#define SDFMT_TOUCH_INIT_TIMEOUT_MS 3000
#define SDFMT_POLL_INTERVAL_MS      20

// ----------------------------------------------------------------------------
// State. Written by the driving task (main task in Case A, touch task /
// worker in Case B), read by the render producer task — single volatile
// fields, no lock needed for display purposes.
// ----------------------------------------------------------------------------

static volatile sd_format_phase_t s_phase = SD_FORMAT_IDLE;
static volatile sd_format_origin_t s_origin = SD_FORMAT_ORIGIN_FATAL;
static volatile int64_t s_countdown_deadline_us = 0;
static volatile int64_t s_notice_deadline_us = 0;
static volatile bool s_fatal_button_ready = false;

// Fatal-screen text, kept for cancel-restore (string literals from app_lcd_p4)
static const char *s_fatal_title = NULL;
static const char *s_fatal_body = NULL;

static esp_lcd_touch_handle_t s_tp = NULL;

// ----------------------------------------------------------------------------
// Getters (render producer + touch router)
// ----------------------------------------------------------------------------

bool sd_format_is_active(void)
{
    return s_phase != SD_FORMAT_IDLE;
}

bool sd_format_owns_screen(void)
{
    switch (s_phase) {
        case SD_FORMAT_WARNING:
        case SD_FORMAT_FORMATTING:
        case SD_FORMAT_FAILED:
        case SD_FORMAT_REBOOTING:
            return true;
        default:
            // PROBING/NO_CARD/CARD_OK render as notices on the fatal screen
            return false;
    }
}

sd_format_phase_t sd_format_get_phase(void)
{
    return s_phase;
}

sd_format_origin_t sd_format_get_origin(void)
{
    return s_origin;
}

int sd_format_get_countdown_ms(void)
{
    int64_t remaining_us = s_countdown_deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    return (int)(remaining_us / 1000);
}

bool sd_format_fatal_button_ready(void)
{
    return s_fatal_button_ready;
}

// ----------------------------------------------------------------------------
// Format execution helpers
// ----------------------------------------------------------------------------

/**
 * @brief Set the "P3A" volume label on the freshly formatted (mounted) card
 *
 * Compiled out unless CONFIG_FATFS_USE_LABEL=y (FF_USE_LABEL). Best-effort:
 * a label failure never fails the format.
 */
static void apply_volume_label(void)
{
#if FF_USE_LABEL
    if (bsp_sdcard == NULL) {
        return;
    }
    BYTE pdrv = ff_diskio_get_pdrv_card(bsp_sdcard);
    if (pdrv == 0xFF) {
        return;
    }
    char label[8];
    snprintf(label, sizeof(label), "%u:P3A", (unsigned)pdrv);
    FRESULT fr = f_setlabel(label);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "volume label not set (FRESULT=%d)", (int)fr);
    }
#endif
}

/**
 * @brief Format an UNMOUNTED card: mount with format_if_mount_failed=true
 *
 * Replicates bsp_sdcard_mount()'s host/slot/mount config exactly (the BSP
 * managed component is read-only and hardcodes format_if_mount_failed from
 * a Kconfig we deliberately keep off). This is the only in-tree path that
 * also rebuilds the partition table (f_fdisk), which truly blank cards
 * need. LDO VO4 is already powered: the boot mount attempt (and any probe)
 * goes through bsp_sdcard_mount(), which acquires it.
 *
 * On success the card is left mounted and bsp_sdcard is set.
 */
static esp_err_t format_by_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,  // scoped to this explicit user action
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    const sdmmc_slot_config_t slot_config = {
        /* SD card is on Slot 0 IO MUX pins — same as the BSP, no pin overrides */
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config,
                                   &mount_config, &bsp_sdcard);
}

// ----------------------------------------------------------------------------
// Flow control
// ----------------------------------------------------------------------------

static void enter_warning(void)
{
    s_countdown_deadline_us = esp_timer_get_time() + (int64_t)SDFMT_COUNTDOWN_MS * 1000;
    s_phase = SD_FORMAT_WARNING;
}

esp_err_t sd_format_start(sd_format_origin_t origin)
{
    if (s_phase != SD_FORMAT_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (origin == SD_FORMAT_ORIGIN_INFO) {
        if (!sd_health_is_failed()) {
            ESP_LOGW(TAG, "refused - sd_health latch not tripped");
            return ESP_ERR_INVALID_STATE;
        }
        if (animation_player_is_sd_export_locked()) {
            ESP_LOGW(TAG, "refused - SD card exported over USB");
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGI(TAG, "user-initiated format requested (%s)",
             origin == SD_FORMAT_ORIGIN_INFO ? "info screen" : "fatal screen");
    s_origin = origin;
    enter_warning();
    return ESP_OK;
}

void sd_format_cancel(void)
{
    if (s_phase != SD_FORMAT_WARNING) {
        return;
    }
    ESP_LOGI(TAG, "cancelled");
    s_phase = SD_FORMAT_IDLE;
    if (s_origin == SD_FORMAT_ORIGIN_FATAL && s_fatal_title != NULL) {
        // Restore the fatal screen text (it may have been replaced by a
        // format-failure message on a previous attempt).
        ugfx_ui_show_fatal_error(s_fatal_title, s_fatal_body);
    }
}

void sd_format_handle_long_press(void)
{
    // Cancel from the warning panel; deliberately a no-op during
    // FORMATTING/REBOOTING so the screen cannot be dismissed mid-format.
    sd_format_cancel();
}

/** Case B worker: quiesce → format → reboot. Spawned by the confirm tap. */
static void sd_format_worker(void *arg)
{
    (void)arg;

    // Quiesce playback/downloads. Same primitive USB-MSC export uses before
    // raw sector access: stops the loader, blocks swaps, pauses downloads.
    // Also takes the export lock, which keeps app_lcd_exit_ui_mode() from
    // dismissing the format screen. Proceed even on failure — the sd_health
    // latch already stopped all writers and we reboot right after.
    esp_err_t err = animation_player_begin_sd_export();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quiesce incomplete (%s), formatting anyway", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "formatting");
    err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, bsp_sdcard);
    if (err == ESP_OK) {
        apply_volume_label();
        ESP_LOGI(TAG, "done, rebooting");
        s_notice_deadline_us = esp_timer_get_time() + (int64_t)SDFMT_REBOOT_NOTICE_MS * 1000;
        s_phase = SD_FORMAT_REBOOTING;
        vTaskDelay(pdMS_TO_TICKS(SDFMT_REBOOT_NOTICE_MS));
    } else {
        // A failed format may have recycled the mount context (bsp_sdcard
        // dangling) and the card is in an unknown state — rebooting is the
        // only safe continuation. A truly dead card lands on the fatal
        // screen next boot, which is the correct end state.
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        s_phase = SD_FORMAT_FAILED;
        vTaskDelay(pdMS_TO_TICKS(SDFMT_FAILED_NOTICE_MS));
        ESP_LOGW(TAG, "rebooting after failed format");
    }
    esp_restart();
}

bool sd_format_handle_tap(uint16_t x, uint16_t y)
{
    switch (s_phase) {
        case SD_FORMAT_WARNING:
            if (x >= SDFMT_BTN_CANCEL_X && x < SDFMT_BTN_CANCEL_X + SDFMT_BTN_CANCEL_W &&
                y >= SDFMT_BTN_CANCEL_Y && y < SDFMT_BTN_CANCEL_Y + SDFMT_BTN_CANCEL_H) {
                sd_format_cancel();
                return true;
            }
            if (x >= SDFMT_BTN_CONFIRM_X && x < SDFMT_BTN_CONFIRM_X + SDFMT_BTN_CONFIRM_W &&
                y >= SDFMT_BTN_CONFIRM_Y && y < SDFMT_BTN_CONFIRM_Y + SDFMT_BTN_CONFIRM_H &&
                sd_format_get_countdown_ms() <= 0) {
                if (s_origin == SD_FORMAT_ORIGIN_INFO) {
                    // The card may have been exported over USB while the
                    // warning panel was up (the USB notice owns the screen
                    // in that case) — never confirm blind.
                    if (animation_player_is_sd_export_locked()) {
                        ESP_LOGW(TAG, "confirm refused - SD card exported over USB");
                        sd_format_cancel();
                        return true;
                    }
                    s_phase = SD_FORMAT_FORMATTING;
                    if (xTaskCreate(sd_format_worker, "sd_format", 8192, NULL, 5, NULL) != pdPASS) {
                        ESP_LOGE(TAG, "failed to spawn format worker");
                        s_phase = SD_FORMAT_WARNING;
                        sd_format_cancel();
                    }
                } else {
                    // Case A: the fatal-screen loop performs the format
                    // inline when it observes this phase change.
                    s_phase = SD_FORMAT_FORMATTING;
                }
                return true;
            }
            // Consume all other taps while the panel is up
            return true;

        case SD_FORMAT_PROBING:
        case SD_FORMAT_NO_CARD:
        case SD_FORMAT_CARD_OK:
        case SD_FORMAT_FORMATTING:
        case SD_FORMAT_FAILED:
        case SD_FORMAT_REBOOTING:
            return true;  // consume, ignore

        case SD_FORMAT_IDLE:
        default:
            return false;  // not ours (fatal-button taps are handled by the loop)
    }
}

// ----------------------------------------------------------------------------
// Case A: fatal-screen interactive loop
// ----------------------------------------------------------------------------

#if P3A_HAS_TOUCH

typedef struct {
    SemaphoreHandle_t done_sem;
    esp_err_t result;
} fatal_touch_init_ctx_t;

/**
 * @brief Sacrificial touch-init task (pattern: app_touch.c touch_init_task)
 *
 * GT911 I2C init can wedge; doing it on a disposable task with a timeout
 * keeps the fatal screen rendering (this task and the render tasks share
 * priority 5, so a spinning init still time-slices). No CPU1 rescue-reboot
 * here — a reboot loop in a persistent no-SD state would be worse than a
 * button-less fatal screen.
 */
static void fatal_touch_init_task(void *arg)
{
    fatal_touch_init_ctx_t *ctx = arg;
    ctx->result = p3a_board_touch_init(&s_tp);
    xSemaphoreGive(ctx->done_sem);
    vTaskDelete(NULL);
}

static bool fatal_touch_init(void)
{
    fatal_touch_init_ctx_t ctx = {
        .done_sem = xSemaphoreCreateBinary(),
        .result = ESP_FAIL,
    };
    if (ctx.done_sem == NULL) {
        return false;
    }
    if (xTaskCreate(fatal_touch_init_task, "sdf_touch_init", 4096, &ctx, 5, NULL) != pdPASS) {
        vSemaphoreDelete(ctx.done_sem);
        return false;
    }
    BaseType_t got = xSemaphoreTake(ctx.done_sem, pdMS_TO_TICKS(SDFMT_TOUCH_INIT_TIMEOUT_MS));
    if (got != pdTRUE) {
        // Init task wedged: leak the semaphore (the task still holds a
        // pointer to it) and give up on touch. 88 bytes lost, once, in a
        // terminal state.
        ESP_LOGE(TAG, "touch init timed out");
        return false;
    }
    vSemaphoreDelete(ctx.done_sem);
    return ctx.result == ESP_OK;
}

/** Probe on initiator tap: decides no-card / good-card / needs-format. */
static void run_probe(void)
{
    s_phase = SD_FORMAT_PROBING;

    if (bsp_sdcard != NULL) {
        // Card mounted at boot but a later init step failed — formatting is
        // still a valid escape hatch, and probing again would double-mount.
        ESP_LOGI(TAG, "probe ok (card already mounted)");
        enter_warning();
        return;
    }

    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        // A usable card appeared since boot (user swapped cards) — no
        // format needed, a clean boot will pick it up.
        ESP_LOGI(TAG, "probe ok - card mounts cleanly, rebooting into normal use");
        s_notice_deadline_us = esp_timer_get_time() + (int64_t)SDFMT_CARD_OK_NOTICE_MS * 1000;
        s_phase = SD_FORMAT_CARD_OK;
    } else if (err == ESP_FAIL) {
        // Exactly ESP_FAIL = card answered but f_mount failed (no/foreign
        // filesystem: blank, exFAT, NTFS, corrupt FAT) — the format case.
        ESP_LOGI(TAG, "probe: card present, filesystem unusable");
        enter_warning();
    } else {
        // Card-init-level error (ESP_ERR_TIMEOUT etc.): nothing in the slot
        // or the card is electrically dead.
        ESP_LOGW(TAG, "probe: no card detected (%s)", esp_err_to_name(err));
        s_notice_deadline_us = esp_timer_get_time() + (int64_t)SDFMT_NO_CARD_NOTICE_MS * 1000;
        s_phase = SD_FORMAT_NO_CARD;
    }
}

/** Blocking format for Case A (runs inline on the main task). */
static void run_fatal_format(void)
{
    ESP_LOGI(TAG, "formatting");
    esp_err_t err;
    if (bsp_sdcard != NULL) {
        err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, bsp_sdcard);
        if (err != ESP_OK) {
            // A failed format may recycle the mount context, leaving the
            // handle dangling — never reuse it; the retry re-probes.
            bsp_sdcard = NULL;
        }
    } else {
        err = format_by_mount();
    }

    if (err == ESP_OK) {
        apply_volume_label();
        bsp_sdcard_unmount();  // best-effort clean unmount before reboot
        ESP_LOGI(TAG, "done, rebooting");
        s_notice_deadline_us = esp_timer_get_time() + (int64_t)SDFMT_REBOOT_NOTICE_MS * 1000;
        s_phase = SD_FORMAT_REBOOTING;
    } else {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        s_phase = SD_FORMAT_IDLE;
        ugfx_ui_show_fatal_error(
            "SD Card Format Failed",
            "Format failed - the card may be\ndefective.\n\nReplace the card, or tap below\nto try again.");
        // The format button stays armed: a retry starts over from the probe.
    }
}

static void handle_fatal_tap(uint16_t x, uint16_t y)
{
    if (s_phase == SD_FORMAT_IDLE) {
        if (x >= SDFMT_BTN_FATAL_X && x < SDFMT_BTN_FATAL_X + SDFMT_BTN_FATAL_W &&
            y >= SDFMT_BTN_FATAL_Y && y < SDFMT_BTN_FATAL_Y + SDFMT_BTN_FATAL_H) {
            ESP_LOGI(TAG, "user-initiated format requested (fatal screen)");
            run_probe();
        }
        return;
    }
    sd_format_handle_tap(x, y);
}

void sd_format_fatal_screen_loop(const char *fatal_title, const char *fatal_body)
{
    s_fatal_title = fatal_title;
    s_fatal_body = fatal_body;
    s_origin = SD_FORMAT_ORIGIN_FATAL;

    if (!fatal_touch_init()) {
        ESP_LOGW(TAG, "touch unavailable, format button disabled");
        vTaskSuspend(NULL);
        for (;;) {
            vTaskDelay(portMAX_DELAY);  // unreachable belt-and-braces
        }
    }

    s_fatal_button_ready = true;
    ESP_LOGI(TAG, "fatal-screen format button armed");

    // The button exists now — swap in the body text that offers it (also
    // becomes the cancel-restore text from here on).
    s_fatal_body = SDFMT_FATAL_BODY_WITH_BUTTON;
    ugfx_ui_show_fatal_error(s_fatal_title, s_fatal_body);

    // Minimal tap detector: same dead-zone threshold as app_touch.c (6.5%
    // of the smaller screen dimension), press-start coords as tap position.
    const uint32_t tap_threshold =
        ((uint32_t)MIN(P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT) * 65U + 500U) / 1000U;
    esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t touch_count = 0;
    bool touch_down = false;
    uint16_t start_x = 0, start_y = 0;
    uint32_t max_movement = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SDFMT_POLL_INTERVAL_MS));
        int64_t now = esp_timer_get_time();

        // Phase timers / blocking work
        switch (s_phase) {
            case SD_FORMAT_NO_CARD:
                if (now >= s_notice_deadline_us) {
                    s_phase = SD_FORMAT_IDLE;
                }
                break;
            case SD_FORMAT_CARD_OK:
                if (now >= s_notice_deadline_us) {
                    bsp_sdcard_unmount();  // best-effort
                    esp_restart();
                }
                break;
            case SD_FORMAT_REBOOTING:
                if (now >= s_notice_deadline_us) {
                    esp_restart();
                }
                break;
            case SD_FORMAT_FORMATTING:
                // Set by the confirm tap; blocks here while the render task
                // keeps showing the "Formatting..." panel.
                run_fatal_format();
                continue;
            default:
                break;
        }

        // Touch sampling (pattern: app_touch.c poll loop)
        esp_lcd_touch_read_data(s_tp);
        esp_err_t terr = esp_lcd_touch_get_data(s_tp, points, &touch_count,
                                                CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
        bool pressed = (terr == ESP_OK && touch_count > 0);

        if (pressed) {
            if (!touch_down) {
                touch_down = true;
                start_x = points[0].x;
                start_y = points[0].y;
                max_movement = 0;
            } else {
                uint32_t dx = (points[0].x > start_x) ? (points[0].x - start_x)
                                                      : (start_x - points[0].x);
                uint32_t dy = (points[0].y > start_y) ? (points[0].y - start_y)
                                                      : (start_y - points[0].y);
                if (dx + dy > max_movement) {
                    max_movement = dx + dy;
                }
            }
        } else if (touch_down) {
            touch_down = false;
            if (max_movement <= tap_threshold) {
                uint16_t tx = start_x;
                uint16_t ty = start_y;
                app_touch_transform_coordinates(&tx, &ty,
                                                display_renderer_get_rotation());
                handle_fatal_tap(tx, ty);
            }
        }
    }
}

#else  // !P3A_HAS_TOUCH

void sd_format_fatal_screen_loop(const char *fatal_title, const char *fatal_body)
{
    (void)fatal_title;
    (void)fatal_body;
    ESP_LOGW(TAG, "no touch support, format button unavailable");
    vTaskSuspend(NULL);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

#endif  // P3A_HAS_TOUCH
