// SPDX-License-Identifier: Apache-2.0
// The SD card as a USB drive. See p3a_usb.h.
//
// The session's rules are usb_session.c's. Here: TinyUSB brought up as a
// serial port plus a mass storage device, the computer's reads and writes
// turned into raw sector transfers on the card, and the session's commands
// carried out.

#include "p3a_usb.h"

#include <stdatomic.h>
#include <string.h>

#include "class/cdc/cdc_device.h"
#include "class/msc/msc_device.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_board.h"
#include "p3a_job.h"
#include "p3a_storage.h"
#include "p3a_ui_hooks.h"
#include "play_scheduler.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "usb_session.h"

static const char *TAG = "usb";

#define TICK_MS 250

static SemaphoreHandle_t s_lock;       // guards the session
static SemaphoreHandle_t s_card_lock;  // one transfer at a time
static usb_session_t s_session;
static atomic_bool s_active;           // the computer may read and write (a copy for the transfer path)
static atomic_bool s_taken;
static sdmmc_card_t *s_card;           // set when a session begins
static uint8_t *s_sector;              // for transfers that are not whole sectors
static uint16_t s_block_size = 512;
static uint32_t s_block_count;
static esp_timer_handle_t s_tick;
static bool s_started;

bool p3a_usb_is_sharing(void)
{
    return atomic_load(&s_taken);
}

// --- carrying out the session's commands ------------------------------------------

// A job: the card is taken from the device. Playback stops for good, the
// storage layer refuses writes from here on, and the screen says why.
static esp_err_t take_card_job(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "a computer has the SD card; the device restarts when it lets go");
    play_scheduler_shutdown();
    p3a_storage_hand_over();
    p3a_ui_content_t words;
    p3a_ui_content(&words, "USB", -1, "The SD card is in use by the computer.\n\nEject it there before unplugging.");
    p3a_ui_screen_show(P3A_SCREEN_USB, &words);
    return ESP_OK;
}

static void run(usb_command_t command)
{
    switch (command) {
    case USB_DO_TAKE_CARD:
        // The transfers are refused until the job has stopped playback: the
        // computer waits ("not ready") rather than share the card.
        if (p3a_job_submit(P3A_JOBS_UI, "usb: take the card", take_card_job, NULL, NULL) != ESP_OK) {
            take_card_job(NULL);
        }
        break;
    case USB_DO_ANNOUNCE_RESTART: {
        ESP_LOGW(TAG, "the USB session is over; restarting to read the card afresh");
        p3a_ui_content_t words;
        p3a_ui_content(&words, "USB", -1, "Restarting to read the card again");
        p3a_ui_screen_show(P3A_SCREEN_USB, &words);
        break;
    }
    case USB_DO_RESTART:
        esp_restart();  // the end of every session, by design: not a remedy, not counted
        break;
    default:
        break;
    }
}

// Runs a change of the session under its lock and carries out the result.
#define SESSION(call)                                \
    do {                                             \
        xSemaphoreTake(s_lock, portMAX_DELAY);       \
        const usb_command_t command_ = (call);       \
        atomic_store(&s_active, s_session.active);   \
        atomic_store(&s_taken, s_session.taken);     \
        xSemaphoreGive(s_lock);                      \
        run(command_);                               \
    } while (0)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static usb_command_t none_after(void)
{
    return USB_DO_NOTHING;
}

// Reads the card's size and makes the scratch sector. False when there is
// no card to share.
static bool card_ready(void)
{
    sdmmc_card_t *card = p3a_board_sd_card();
    if (card == NULL) {
        return false;
    }
    s_card = card;
    s_block_size = card->csd.sector_size ? (uint16_t)card->csd.sector_size : 512;
    s_block_count = (uint32_t)card->csd.capacity;
    if (s_sector == NULL) {
        // DMA-capable internal memory: the SD driver reads straight into it.
        s_sector = heap_caps_aligned_alloc(64, 4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    return s_sector != NULL && s_block_size <= 4096;
}

void p3a_usb_session_event(p3a_usb_event_t event)
{
    switch (event) {
    case P3A_USB_MOUNTED:
        if (!card_ready()) {
            ESP_LOGW(TAG, "a computer is connected, but there is no card to share");
            return;
        }
        SESSION(usb_session_mounted(&s_session, now_ms()));
        break;
    case P3A_USB_UNMOUNTED:
        SESSION((usb_session_unmounted(&s_session, now_ms()), none_after()));
        break;
    case P3A_USB_SUSPENDED:
        SESSION((usb_session_suspended(&s_session, now_ms()), none_after()));
        break;
    case P3A_USB_RESUMED:
        SESSION((usb_session_resumed(&s_session, tud_mounted()), none_after()));
        break;
    }
}

static void tick(void *arg)
{
    (void)arg;
    SESSION(usb_session_tick(&s_session, now_ms(), s_started && tud_ready() && tud_mounted()));
}

// --- TinyUSB's callbacks: the bus ----------------------------------------------------

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "configured by a computer");
    p3a_usb_session_event(P3A_USB_MOUNTED);
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "disconnected");
    p3a_usb_session_event(P3A_USB_UNMOUNTED);
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    p3a_usb_session_event(P3A_USB_SUSPENDED);
}

void tud_resume_cb(void)
{
    p3a_usb_session_event(P3A_USB_RESUMED);
}

// The serial port exists so that the device keeps the interface numbers 1.x
// had; nothing is said over it and what arrives is thrown away.
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    uint8_t discard[64];
    while (tud_cdc_available() && tud_cdc_read(discard, sizeof(discard)) > 0) {
    }
}

// --- TinyUSB's callbacks: mass storage ----------------------------------------------

// The computer may transfer only while a session is active and the card has
// really been taken from the device (playback stopped, writes refused).
static bool may_transfer(void)
{
    return atomic_load(&s_active) && s_card != NULL && p3a_storage_is_handed_over();
}

static int32_t transfer(bool write, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!may_transfer() || buffer == NULL || bufsize == 0 || offset >= s_block_size) {
        return -1;
    }
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    const size_t block = s_block_size;
    size_t left = bufsize, in_sector = offset;
    uint8_t *at = buffer;
    uint32_t sector = lba;
    while (left > 0 && err == ESP_OK) {
        if (in_sector == 0 && left >= block) {
            // Whole sectors go straight between the computer's buffer and the card.
            const size_t count = left / block;
            err = write ? sdmmc_write_sectors(s_card, at, sector, count) : sdmmc_read_sectors(s_card, at, sector, count);
            at += count * block;
            left -= count * block;
            sector += (uint32_t)count;
            continue;
        }
        // Part of a sector: through the scratch sector.
        const size_t chunk = left < block - in_sector ? left : block - in_sector;
        err = sdmmc_read_sectors(s_card, s_sector, sector, 1);
        if (err == ESP_OK && write) {
            memcpy(s_sector + in_sector, at, chunk);
            err = sdmmc_write_sectors(s_card, s_sector, sector, 1);
        } else if (err == ESP_OK) {
            memcpy(at, s_sector + in_sector, chunk);
        }
        at += chunk;
        left -= chunk;
        in_sector += chunk;
        if (in_sector >= block) {
            in_sector = 0;
            sector++;
        }
    }
    xSemaphoreGive(s_card_lock);
    return err == ESP_OK ? (int32_t)bufsize : -1;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "ESP32   ", 8);
    memcpy(product_id, "P3A SD CARD     ", 16);
    memcpy(product_rev, "2.0 ", 4);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_block_count;
    *block_size = s_block_size;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!may_transfer()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  // "medium not present"
        return false;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    return transfer(false, lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    return transfer(true, lba, offset, buffer, bufsize);
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
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

// --- start ------------------------------------------------------------------------------

esp_err_t p3a_usb_start(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_card_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_card_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    usb_session_init(&s_session);
    const esp_timer_create_args_t timer = {.callback = tick, .name = "usb_tick"};
    esp_err_t err = esp_timer_create(&timer, &s_tick);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_tick, TICK_MS * 1000);
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t strings = 0;
    const char **table = usb_desc_get_string_table(&strings);
    const tinyusb_config_t config = {
        .device_descriptor = usb_desc_get_device(),
        .string_descriptor = table,
        .string_descriptor_count = (int)strings,
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
    // The P4 has only its UTMI PHY and esp_tinyusb always asks for the
    // internal one, which logs a warning that means nothing here.
    const esp_log_level_t phy_level = esp_log_level_get("usb_phy");
    esp_log_level_set("usb_phy", ESP_LOG_ERROR);
    err = tinyusb_driver_install(&config);
    esp_log_level_set("usb_phy", phy_level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB did not start: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB ready: the SD card is offered to a computer that connects");
    return ESP_OK;
}
