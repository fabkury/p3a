// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file app_usb.c
 * @brief USB composite device: Mass Storage (SD card), CDC serial, and PICO-8 vendor endpoint
 */

#include "app_usb.h"

#if CONFIG_P3A_USB_MSC_ENABLE

#include <stdlib.h>
#include <string.h>

#include "animation_player.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tusb.h"

#include "class/cdc/cdc_device.h"
#include "class/msc/msc_device.h"
#include "usb_descriptors.h"
#include "ugfx_ui.h"
#include "app_lcd.h"

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
#include "pico8_stream.h"
#endif

static const char *TAG = "app_usb";

extern sdmmc_card_t *bsp_sdcard;

static SemaphoreHandle_t s_msc_mutex = NULL;
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
static SemaphoreHandle_t s_vendor_mutex = NULL;
#endif
static uint8_t *s_sector_buffer = NULL;
static size_t s_sector_buffer_size = 0;
static uint16_t s_block_size = 512;
static uint32_t s_block_count = 0;
static bool s_usb_active = false;

// Set-once latch: the USB host issued at least one write to the card during
// this power cycle. FATFS stays mounted throughout the export, so any host
// write leaves its in-RAM state (FAT window, directory sectors, FSINFO free
// count) stale — the file-list refresh on detach would then operate on
// garbage (worst case: host reformatted the card and downloads scribble over
// the new volume). A session that wrote therefore always ends in esp_restart()
// into a fresh mount; the latch is deliberately never cleared.
static volatile bool s_host_wrote = false;
// The reboot notice is on screen and esp_restart() is imminent — refuse any
// late MSC re-activation.
static volatile bool s_reboot_pending = false;

// USB enumeration can briefly bounce on physical cable removal: the bus reaches
// CONFIGURED for a few hundred ms before the disconnect is finalized. Suppress
// any tud_mount_cb that fires within this window after a confirmed unmount so
// playback isn't paused twice and the LCD doesn't flash to UI mode and back.
// Bus suspend is NOT treated as a disconnect — without VBUS detection it can
// fire for benign reasons (host idle, enumeration thrash on fresh plug-in)
// and would otherwise poison the next legitimate mount.
#define USB_MSC_REMOUNT_DEBOUNCE_US (1500 * 1000)
#define USB_MSC_MOUNT_RECOVERY_SLACK_US (200 * 1000)
static int64_t s_last_unmount_us = 0;
static esp_timer_handle_t s_mount_recovery_timer = NULL;

// After a detach/suspend with host writes pending, wait out this settle window
// before rebooting: if the bus was only bouncing, the mount-recovery path
// re-activates the export and the reboot is cancelled. Must stay longer than
// USB_MSC_REMOUNT_DEBOUNCE_US + USB_MSC_MOUNT_RECOVERY_SLACK_US so recovery
// always wins the race against the settle timer.
#define USB_MSC_REBOOT_SETTLE_US (USB_MSC_REMOUNT_DEBOUNCE_US + 500 * 1000)
// How long the "card updated — restarting" notice stays up before esp_restart().
#define USB_MSC_REBOOT_NOTICE_MS 4000
static esp_timer_handle_t s_reboot_settle_timer = NULL;

static esp_err_t update_card_capacity(void);
static int32_t msc_handle_transfer(bool write, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
static void perform_mount_activation(void);
static void mount_recovery_timer_cb(void *arg);
static void mount_recovery_worker(void *arg);
static void reboot_settle_timer_cb(void *arg);
static void spawn_reboot_worker(void);

esp_err_t app_usb_init(void)
{
    if (s_msc_mutex) {
        return ESP_OK;
    }

    s_msc_mutex = xSemaphoreCreateMutex();
    if (!s_msc_mutex) {
        ESP_LOGE(TAG, "Failed to create MSC mutex");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    s_vendor_mutex = xSemaphoreCreateMutex();
    if (!s_vendor_mutex) {
        ESP_LOGE(TAG, "Failed to create vendor mutex");
        vSemaphoreDelete(s_msc_mutex);
        s_msc_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
#endif

    const esp_timer_create_args_t recovery_timer_args = {
        .callback = mount_recovery_timer_cb,
        .name = "usb_msc_recover",
    };
    esp_err_t timer_err = esp_timer_create(&recovery_timer_args, &s_mount_recovery_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGW(TAG, "Mount recovery timer unavailable: %s", esp_err_to_name(timer_err));
        s_mount_recovery_timer = NULL;
    }

    const esp_timer_create_args_t reboot_timer_args = {
        .callback = reboot_settle_timer_cb,
        .name = "usb_msc_reboot",
    };
    timer_err = esp_timer_create(&reboot_timer_args, &s_reboot_settle_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGW(TAG, "Reboot settle timer unavailable: %s", esp_err_to_name(timer_err));
        s_reboot_settle_timer = NULL;
    }

    size_t string_count = 0;
    const char **string_table = usb_desc_get_string_table(&string_count);

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = usb_desc_get_device(),
        .string_descriptor = string_table,
        .string_descriptor_count = string_count,
        .external_phy = false,
#if TUD_OPT_HIGH_SPEED
        .fs_configuration_descriptor = usb_desc_get_fs_configuration(),
        .hs_configuration_descriptor = usb_desc_get_hs_configuration(),
        .qualifier_descriptor = usb_desc_get_qualifier(),
#else
        .configuration_descriptor = usb_desc_get_fs_configuration(),
#endif
        .self_powered = true,
        .vbus_monitor_io = -1,
    };

    // ESP32-P4 only has UTMI PHY, but esp_tinyusb always requests internal PHY,
    // causing a harmless "Using UTMI PHY instead of requested internal PHY" warning.
    // Suppress it here since there is no API to specify UTMI directly.
    esp_log_level_t prev_phy_level = esp_log_level_get("usb_phy");
    esp_log_level_set("usb_phy", ESP_LOG_ERROR);
    esp_err_t tusb_err = tinyusb_driver_install(&tusb_cfg);
    esp_log_level_set("usb_phy", prev_phy_level);
    ESP_RETURN_ON_ERROR(tusb_err, TAG, "Failed to install TinyUSB");
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    ESP_RETURN_ON_ERROR(pico8_stream_init(), TAG, "Failed to start PICO-8 stream task");
#endif

    ESP_LOGI(TAG, "TinyUSB composite device initialized");
    return ESP_OK;
}

bool app_usb_is_stream_active(void)
{
    return s_usb_active && tud_ready();
}

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
void app_usb_report_touch(const pico8_touch_report_t *report)
{
    if (!report || !s_usb_active || !tud_ready()) {
        return;
    }

    if (tud_vendor_write_available() < sizeof(*report)) {
        return;
    }

    if (s_vendor_mutex && xSemaphoreTake(s_vendor_mutex, 0) != pdTRUE) {
        return;
    }

    tud_vendor_write(report, sizeof(*report));
    tud_vendor_flush();

    if (s_vendor_mutex) {
        xSemaphoreGive(s_vendor_mutex);
    }
}
#endif

static esp_err_t update_card_capacity(void)
{
    if (!bsp_sdcard) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_block_size = (uint16_t)bsp_sdcard->csd.sector_size;
    if (s_block_size == 0) {
        s_block_size = 512;
    }
    s_block_count = (uint32_t)bsp_sdcard->csd.capacity;

    if (s_sector_buffer_size < s_block_size) {
        uint8_t *new_buffer = (uint8_t *)realloc(s_sector_buffer, s_block_size);
        if (!new_buffer) {
            ESP_LOGE(TAG, "Failed to allocate sector buffer (%u bytes)", s_block_size);
            return ESP_ERR_NO_MEM;
        }
        s_sector_buffer = new_buffer;
        s_sector_buffer_size = s_block_size;
    }

    ESP_LOGI(TAG, "SD capacity: %u blocks x %u bytes", (unsigned)s_block_count, (unsigned)s_block_size);
    return ESP_OK;
}

static int32_t msc_handle_transfer(bool write, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!s_usb_active || !bsp_sdcard || !buffer || bufsize == 0) {
        return -1;
    }

    // Latch on the attempt, not the outcome: even a partial write poisons the
    // stale FATFS view of the card.
    if (write && !s_host_wrote) {
        s_host_wrote = true;
        ESP_LOGI(TAG, "USB host wrote to SD card - device will restart when the session ends");
    }

    if (offset >= s_block_size) {
        ESP_LOGW(TAG, "MSC transfer offset out of range (offset=%u)", (unsigned)offset);
        return -1;
    }

    if (!s_sector_buffer) {
        ESP_LOGE(TAG, "Sector buffer unavailable");
        return -1;
    }

    if (xSemaphoreTake(s_msc_mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    esp_err_t err = ESP_OK;
    const size_t block_size = s_block_size;
    size_t remaining = bufsize;
    uint8_t *buf_ptr = buffer;
    uint32_t current_lba = lba;
    size_t sector_offset = offset;

    while (remaining > 0 && err == ESP_OK) {
        if (sector_offset == 0 && remaining >= block_size) {
            size_t whole_blocks = remaining / block_size;
            size_t block_bytes = whole_blocks * block_size;
            if (!write) {
                err = sdmmc_read_sectors(bsp_sdcard, buf_ptr, current_lba, whole_blocks);
            } else {
                err = sdmmc_write_sectors(bsp_sdcard, buf_ptr, current_lba, whole_blocks);
            }
            if (err != ESP_OK) {
                break;
            }
            buf_ptr += block_bytes;
            remaining -= block_bytes;
            current_lba += whole_blocks;
            continue;
        }

        const size_t sector_space = block_size - sector_offset;
        size_t chunk = (remaining < sector_space) ? remaining : sector_space;

        if (!write) {
            if (sector_offset == 0 && chunk == block_size) {
                err = sdmmc_read_sectors(bsp_sdcard, buf_ptr, current_lba, 1);
            } else {
                err = sdmmc_read_sectors(bsp_sdcard, s_sector_buffer, current_lba, 1);
                if (err == ESP_OK) {
                    memcpy(buf_ptr, s_sector_buffer + sector_offset, chunk);
                }
            }
        } else {
            if (sector_offset == 0 && chunk == block_size) {
                err = sdmmc_write_sectors(bsp_sdcard, buf_ptr, current_lba, 1);
            } else {
                err = sdmmc_read_sectors(bsp_sdcard, s_sector_buffer, current_lba, 1);
                if (err == ESP_OK) {
                    memcpy(s_sector_buffer + sector_offset, buf_ptr, chunk);
                    err = sdmmc_write_sectors(bsp_sdcard, s_sector_buffer, current_lba, 1);
                }
            }
        }

        if (err != ESP_OK) {
            break;
        }

        buf_ptr += chunk;
        remaining -= chunk;
        sector_offset += chunk;
        if (sector_offset >= block_size) {
            sector_offset -= block_size;
            current_lba++;
        }
    }

    xSemaphoreGive(s_msc_mutex);
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

static void perform_mount_activation(void)
{
    if (s_reboot_pending) {
        ESP_LOGW(TAG, "Ignoring USB mount: restart notice active");
        return;
    }
    // A re-mount inside the settle window means the detach was a bounce - the
    // session continues and the pending write-triggered reboot is cancelled.
    if (s_reboot_settle_timer) {
        esp_timer_stop(s_reboot_settle_timer);
    }

    ESP_LOGI(TAG, "USB host mounted");
    esp_err_t err = animation_player_begin_sd_export();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to export SD card: %s", esp_err_to_name(err));
        s_usb_active = false;
        return;
    }

    err = update_card_capacity();
    if (err != ESP_OK) {
        animation_player_end_sd_export();
        s_usb_active = false;
        return;
    }

    s_usb_active = true;
    app_lcd_enter_ui_mode();
    ugfx_ui_show_usb_msc();
}

static void mount_recovery_worker(void *arg)
{
    (void)arg;
    if (tud_ready() && tud_mounted() && !s_usb_active) {
        ESP_LOGW(TAG, "Mount recovery: re-activating MSC after debounced mount");
        s_last_unmount_us = 0;
        perform_mount_activation();
    }
    vTaskDelete(NULL);
}

// The host wrote to the card and the session is over (detach or suspend
// outlasted the settle window). The still-mounted FATFS view is stale, so it
// must never be touched again: the SD-export lock stays held (no file-list
// refresh, downloads stay paused, the screen stays modal), the restart notice
// is shown, and the device reboots into a fresh mount.
static void reboot_worker(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "USB host wrote to SD card - restarting to reload it");
    ugfx_ui_show_usb_msc_reboot();
    vTaskDelay(pdMS_TO_TICKS(USB_MSC_REBOOT_NOTICE_MS));
    esp_restart();
}

static void spawn_reboot_worker(void)
{
    if (s_reboot_pending) {
        return;
    }
    s_reboot_pending = true;
    if (xTaskCreate(reboot_worker, "usb_msc_reboot", 4096, NULL,
                    tskIDLE_PRIORITY + 5, NULL) != pdPASS) {
        // No memory for the notice path - restart anyway, correctness first.
        ESP_LOGE(TAG, "Failed to spawn reboot worker - restarting immediately");
        esp_restart();
    }
}

// Runs on the esp_timer task; must not block.
static void reboot_settle_timer_cb(void *arg)
{
    (void)arg;
    if (s_usb_active || !s_host_wrote) {
        return;  // session resumed (bounce re-mount or bus resume)
    }
    spawn_reboot_worker();
}

// Runs on the esp_timer task; must not block. The activation itself can wait
// on the loader semaphore, so it is deferred to a one-shot worker task.
static void mount_recovery_timer_cb(void *arg)
{
    (void)arg;
    if (s_usb_active) {
        return;
    }
    if (!tud_ready() || !tud_mounted()) {
        ESP_LOGD(TAG, "Mount recovery: device no longer mounted");
        return;
    }
    BaseType_t r = xTaskCreate(mount_recovery_worker, "usb_msc_recv",
                               4096, NULL, tskIDLE_PRIORITY + 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Mount recovery: failed to spawn worker (no memory)");
    }
}

void tud_mount_cb(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_last_unmount_us != 0 &&
        (now_us - s_last_unmount_us) < USB_MSC_REMOUNT_DEBOUNCE_US) {
        ESP_LOGI(TAG, "Ignoring USB mount: bounce %lld ms after unmount",
                 (long long)((now_us - s_last_unmount_us) / 1000));
        // If the host actually keeps us configured (legitimate fresh mount that
        // happened to land in the debounce window), recover after the window
        // closes. Cheap insurance against a stuck "host thinks configured /
        // device thinks unmounted" state.
        if (s_mount_recovery_timer) {
            esp_timer_stop(s_mount_recovery_timer);
            esp_timer_start_once(s_mount_recovery_timer,
                                 USB_MSC_REMOUNT_DEBOUNCE_US + USB_MSC_MOUNT_RECOVERY_SLACK_US);
        }
        return;
    }
    s_last_unmount_us = 0;
    perform_mount_activation();
}

void tud_umount_cb(void)
{
    if (s_mount_recovery_timer) {
        esp_timer_stop(s_mount_recovery_timer);
    }
    s_last_unmount_us = esp_timer_get_time();
    ESP_LOGI(TAG, "USB host disconnected");
    s_usb_active = false;
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    pico8_stream_reset();
#endif
    if (s_host_wrote) {
        // The host changed the card under the still-mounted (now stale) FATFS,
        // so the normal end-of-export refresh must NOT run. Keep the SD-export
        // lock held so nothing touches the stale view, keep the modal screen
        // up, and reboot once the bus has settled — a bounce re-mount within
        // the window cancels the reboot via perform_mount_activation().
        if (s_reboot_settle_timer) {
            esp_timer_stop(s_reboot_settle_timer);
            esp_timer_start_once(s_reboot_settle_timer, USB_MSC_REBOOT_SETTLE_US);
        } else {
            spawn_reboot_worker();
        }
        return;
    }
    // Release the SD-export lock (and refresh the local file view) BEFORE
    // leaving UI mode. While the lock is held, exit-UI-mode is intentionally a
    // no-op so the "SD exposed" notice stays modal, so the lock must drop first
    // or the screen would remain stuck on the notice after the host detaches.
    animation_player_end_sd_export();
    ugfx_ui_hide_usb_msc();
    app_lcd_exit_ui_mode();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "USB suspended (remote_wakeup_en=%d)", (int)remote_wakeup_en);
    if (s_mount_recovery_timer) {
        esp_timer_stop(s_mount_recovery_timer);
    }
    // Deliberately do NOT touch s_last_unmount_us here. A bus suspend is not a
    // disconnect; conflating the two caused legitimate mounts to be debounced
    // when a transient suspend fires during the plug-in enumeration cycle.
    s_usb_active = false;
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    pico8_stream_reset();
#endif
    if (s_host_wrote) {
        // Same handling as tud_umount_cb(): a host that wrote and then
        // suspended (e.g. laptop sleep with the cable left in) must also end
        // in a reboot — the FATFS view is just as stale. A transient suspend
        // is cancelled by tud_resume_cb() inside the settle window.
        if (s_reboot_settle_timer) {
            esp_timer_stop(s_reboot_settle_timer);
            esp_timer_start_once(s_reboot_settle_timer, USB_MSC_REBOOT_SETTLE_US);
        } else {
            spawn_reboot_worker();
        }
        return;
    }
    // Drop the SD-export lock before leaving UI mode — see tud_umount_cb().
    animation_player_end_sd_export();
    ugfx_ui_hide_usb_msc();
    app_lcd_exit_ui_mode();
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
    // Resume implies the prior suspend was not a disconnect — clear any stale
    // debounce timestamp so the next mount isn't suppressed.
    s_last_unmount_us = 0;
    // A writes-pending suspend kept the SD-export lock held and only armed the
    // reboot settle timer; a resume inside the window means the session
    // continues — cancel the reboot and let MSC transfers carry on.
    if (s_host_wrote && !s_reboot_pending && tud_mounted() &&
        animation_player_is_sd_export_locked()) {
        if (s_reboot_settle_timer) {
            esp_timer_stop(s_reboot_settle_timer);
        }
        s_usb_active = true;
    }
}

// CDC callbacks
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    uint8_t buf[64];
    while (tud_cdc_available()) {
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        if (count == 0) {
            break;
        }
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    ESP_LOGI(TAG, "CDC line state changed: DTR=%d RTS=%d", dtr, rts);
}

// MSC callbacks
bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    const char vid[] = "ESP32";
    const char pid[] = "P3A SD CARD";
    const char rev[] = "1.0";

    memset(vendor_id, ' ', 8);
    memcpy(vendor_id, vid, MIN(sizeof(vid) - 1, 8U));

    memset(product_id, ' ', 16);
    memcpy(product_id, pid, MIN(sizeof(pid) - 1, 16U));

    memset(product_rev, ' ', 4);
    memcpy(product_rev, rev, MIN(sizeof(rev) - 1, 4U));
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_block_count;
    *block_size = s_block_size;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (!s_usb_active || !bsp_sdcard) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    return msc_handle_transfer(false, lba, offset, (uint8_t *)buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    return msc_handle_transfer(true, lba, offset, buffer, bufsize);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)lun;
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

#else

esp_err_t app_usb_init(void)
{
    return ESP_OK;
}

bool app_usb_is_stream_active(void)
{
    return false;
}

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
void app_usb_report_touch(const pico8_touch_report_t *report)
{
    (void)report;
}
#endif

#endif  // CONFIG_P3A_USB_MSC_ENABLE


