// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_idle_wait.c
 * @brief Yielding replacement for IDF's sdmmc_wait_for_idle() (jitter work stream, fix 8).
 *
 * After every SD write (and single-block bounce writes) the IDF driver waits
 * for the card to leave its busy state by polling CMD13 back-to-back, with no
 * yield during the first 100 ms (components/sdmmc/sdmmc_common.c). A busy
 * period is 1-45 ms on the cards seen in the field, so each write turns into a
 * storm of hundreds of host commands. Measured on the ESP32-P4 dev unit
 * (docs/jitter/runs/RUN-20260830-04-idle-exp.md): during such storms the
 * decode and upscale on BOTH cores run 3-50x slower, which was the dominant
 * source of sporadic 100-800 ms playback stalls. Polling once per FreeRTOS
 * tick removes the effect entirely: the same provocation that produced 6-9
 * stalls per run produces none, at no measurable cost to SD throughput (a
 * 32 KB write still completes in ~5 ms on a healthy card).
 *
 * Linked with --wrap=sdmmc_wait_for_idle (see CMakeLists.txt); the original is
 * kept reachable as __real_sdmmc_wait_for_idle for the SPI-host assert path.
 */

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_private/sdmmc_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t __real_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status);

esp_err_t __wrap_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status)
{
    if (host_is_spi(card)) {
        return __real_sdmmc_wait_for_idle(card, status);
    }
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    uint32_t polls = 0;
    while (!sdmmc_ready_for_data(status)) {
        if (esp_timer_get_time() - t0 > SDMMC_READY_FOR_DATA_TIMEOUT_US) {
            return ESP_ERR_TIMEOUT;
        }
        if (polls++ > 0) {
            vTaskDelay(1);   // one CMD13 per tick instead of a poll storm
        }
        err = sdmmc_send_cmd_send_status(card, &status);
        if (err != ESP_OK) {
            return err;
        }
    }
    return err;
}
