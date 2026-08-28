// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file frame_trace_wraps.c
 * @brief Link-time wrappers (GNU ld --wrap) that timestamp SD-card sector I/O
 *        and SPI-flash operations for the jitter work stream. Diag builds only:
 *        CMakeLists.txt adds the --wrap options iff CONFIG_P3A_FRAME_TRACE, so
 *        release binaries do not contain these symbols at all.
 *
 * Marks: FT_MARK_SD_READ / FT_MARK_SD_WRITE  arg = bytes
 *        FT_MARK_FLASH_OP                     arg = (op << 28) | length, op 1 read, 2 write, 3 erase
 */

#include "frame_trace.h"
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "esp_flash.h"

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_block, size_t block_count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_block, size_t block_count);
esp_err_t __real_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_block, size_t block_count)
{
    frame_trace_mark(FT_MARK_SD_READ, FT_PHASE_BEGIN, (uint32_t)(block_count * 512));
    esp_err_t r = __real_sdmmc_read_sectors(card, dst, start_block, block_count);
    frame_trace_mark(FT_MARK_SD_READ, FT_PHASE_END, (uint32_t)(block_count * 512));
    return r;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_block, size_t block_count)
{
    frame_trace_mark(FT_MARK_SD_WRITE, FT_PHASE_BEGIN, (uint32_t)(block_count * 512));
    esp_err_t r = __real_sdmmc_write_sectors(card, src, start_block, block_count);
    frame_trace_mark(FT_MARK_SD_WRITE, FT_PHASE_END, (uint32_t)(block_count * 512));
    return r;
}

esp_err_t __wrap_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
    frame_trace_mark(FT_MARK_FLASH_OP, FT_PHASE_BEGIN, (1u << 28) | (length & 0x0FFFFFFFu));
    esp_err_t r = __real_esp_flash_read(chip, buffer, address, length);
    frame_trace_mark(FT_MARK_FLASH_OP, FT_PHASE_END, (1u << 28) | (length & 0x0FFFFFFFu));
    return r;
}

esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    frame_trace_mark(FT_MARK_FLASH_OP, FT_PHASE_BEGIN, (2u << 28) | (length & 0x0FFFFFFFu));
    esp_err_t r = __real_esp_flash_write(chip, buffer, address, length);
    frame_trace_mark(FT_MARK_FLASH_OP, FT_PHASE_END, (2u << 28) | (length & 0x0FFFFFFFu));
    return r;
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
    frame_trace_mark(FT_MARK_FLASH_OP, FT_PHASE_BEGIN, (3u << 28) | (len & 0x0FFFFFFFu));
    esp_err_t r = __real_esp_flash_erase_region(chip, start, len);
    frame_trace_mark(FT_MARK_FLASH_OP, FT_PHASE_END, (3u << 28) | (len & 0x0FFFFFFFu));
    return r;
}
