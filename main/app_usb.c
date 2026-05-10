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

static esp_err_t update_card_capacity(void);
static int32_t msc_handle_transfer(bool write, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
static void perform_mount_activation(void);
static void mount_recovery_timer_cb(void *arg);
static void mount_recovery_worker(void *arg);

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
    ugfx_ui_hide_usb_msc();
    app_lcd_exit_ui_mode();
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    pico8_stream_reset();
#endif
    animation_player_end_sd_export();
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
    ugfx_ui_hide_usb_msc();
    app_lcd_exit_ui_mode();
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    pico8_stream_reset();
#endif
    animation_player_end_sd_export();
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
    // Resume implies the prior suspend was not a disconnect — clear any stale
    // debounce timestamp so the next mount isn't suppressed.
    s_last_unmount_us = 0;
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


