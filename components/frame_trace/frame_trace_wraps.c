// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file frame_trace_wraps.c
 * @brief Link-time wrappers (GNU ld --wrap) that timestamp SD-card sector I/O
 *        and SPI-flash operations for the jitter work stream. Diag builds only:
 *        CMakeLists.txt adds the --wrap options iff CONFIG_P3A_FRAME_TRACE, so
 *        release binaries do not contain these symbols at all.
 *
 * Marks (one span entry per op, duration in lateness_us): FT_MARK_SD_READ / FT_MARK_SD_WRITE  arg = bytes
 *        FT_MARK_FLASH_OP                     arg = (op << 28) | length, op 1 read, 2 write, 3 erase
 */

#include "frame_trace.h"
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "esp_flash.h"
#include "esp_private/sdmmc_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_block, size_t block_count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_block, size_t block_count);
esp_err_t __real_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_block, size_t block_count)
{
    const int64_t t0 = frame_trace_now_us();
    esp_err_t r = __real_sdmmc_read_sectors(card, dst, start_block, block_count);
    frame_trace_mark_span(FT_MARK_SD_READ, t0, (uint32_t)(block_count * 512));
    return r;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_block, size_t block_count)
{
    const int64_t t0 = frame_trace_now_us();
    esp_err_t r = __real_sdmmc_write_sectors(card, src, start_block, block_count);
    frame_trace_mark_span(FT_MARK_SD_WRITE, t0, (uint32_t)(block_count * 512));
    return r;
}

esp_err_t __wrap_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
    const int64_t t0 = frame_trace_now_us();
    esp_err_t r = __real_esp_flash_read(chip, buffer, address, length);
    frame_trace_mark_span(FT_MARK_FLASH_OP, t0, (1u << 28) | (length & 0x0FFFFFFFu));
    return r;
}

esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    const int64_t t0 = frame_trace_now_us();
    esp_err_t r = __real_esp_flash_write(chip, buffer, address, length);
    frame_trace_mark_span(FT_MARK_FLASH_OP, t0, (2u << 28) | (length & 0x0FFFFFFFu));
    return r;
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
    const int64_t t0 = frame_trace_now_us();
    esp_err_t r = __real_esp_flash_erase_region(chip, start, len);
    frame_trace_mark_span(FT_MARK_FLASH_OP, t0, (3u << 28) | (len & 0x0FFFFFFFu));
    return r;
}

// ---------------------------------------------------------------------------
// EXPERIMENT (2026-08-30): yielding card-idle wait. IDF's sdmmc_wait_for_idle()
// polls CMD13 back-to-back with no yield for the first 100 ms after every
// write; with this card busy 5-45 ms per 32 KB block, every write became a
// burst of hundreds of commands, and every such burst coincides with both
// cores' decode/upscale running 3-50x slow. This wrapper polls once, then
// sleeps a tick between polls. If the stalls disappear, the poll storm is
// the mechanism and a release fix follows (same wrapper as a p3a component).
// Marks: FT_MARK_USER span, arg = 0x1D1E, duration = time spent waiting.
esp_err_t __real_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status);

esp_err_t __wrap_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status)
{
    const int64_t t0 = frame_trace_now_us();
    esp_err_t err = ESP_OK;
    uint32_t polls = 0;
    while (!sdmmc_ready_for_data(status)) {
        if (frame_trace_now_us() - t0 > SDMMC_READY_FOR_DATA_TIMEOUT_US) {
            frame_trace_mark_span(FT_MARK_USER, t0, 0x1D1E0000u | 0xFFFFu);
            return ESP_ERR_TIMEOUT;
        }
        if (polls++ > 0) {
            vTaskDelay(1);   // let the card work; one CMD13 per tick instead of thousands
        }
        err = sdmmc_send_cmd_send_status(card, &status);
        if (err != ESP_OK) {
            frame_trace_mark_span(FT_MARK_USER, t0, 0x1D1E0000u | (polls & 0xFFFFu));
            return err;
        }
    }
    if (polls > 0) {
        frame_trace_mark_span(FT_MARK_USER, t0, 0x1D1E0000u | (polls & 0xFFFFu));
    }
    return err;
}
