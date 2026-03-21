// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pico8_audio.c
 * @brief PICO-8 audio: ring buffer → ES8311 codec → speaker
 */

#include "pico8_audio.h"

#include <string.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#define TAG "pico8_audio"

#define AUDIO_SAMPLE_RATE   22050
#define AUDIO_CHANNELS      1
#define AUDIO_BITS          16
#define AUDIO_FEED_CHUNK    512   /* bytes per codec write */
#define AUDIO_TASK_STACK    3072
#define AUDIO_TASK_PRIO     (tskIDLE_PRIORITY + 5)

static esp_codec_dev_handle_t s_speaker = NULL;
static StreamBufferHandle_t   s_ring    = NULL;
static TaskHandle_t           s_task    = NULL;
static volatile bool          s_active  = false;
static bool                   s_hw_init = false;

/* ── Hardware init (lazy, once) ── */

static esp_err_t hw_init(void)
{
    if (s_hw_init) return ESP_OK;

    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .sample_rate     = AUDIO_SAMPLE_RATE,
    };
    int ret = esp_codec_dev_open(s_speaker, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        return ESP_FAIL;
    }

    esp_codec_dev_set_out_vol(s_speaker, CONFIG_P3A_PICO8_AUDIO_VOLUME);
    esp_codec_dev_set_out_mute(s_speaker, true);  /* unmute on start */

    s_hw_init = true;
    ESP_LOGI(TAG, "Audio HW init OK (rate=%d vol=%d)", AUDIO_SAMPLE_RATE,
             CONFIG_P3A_PICO8_AUDIO_VOLUME);
    return ESP_OK;
}

/* ── Feed task: ring buffer → codec ── */

static void audio_feed_task(void *arg)
{
    (void)arg;
    uint8_t buf[AUDIO_FEED_CHUNK];

    bool receiving = false;  /* Have we ever received real audio data? */

    while (s_active) {
        size_t got = xStreamBufferReceive(s_ring, buf, sizeof(buf),
                                          pdMS_TO_TICKS(50));
        if (got > 0) {
            receiving = true;
            esp_codec_dev_write(s_speaker, buf, (int)got);
        } else if (s_active && receiving) {
            /* Underrun after we started receiving: feed silence to avoid pops */
            memset(buf, 0, sizeof(buf));
            esp_codec_dev_write(s_speaker, buf, sizeof(buf));
        }
        /* If we never received data, just block on the stream buffer */
    }

    /* Drain: flush a silence chunk to push remaining DMA data through */
    memset(buf, 0, sizeof(buf));
    esp_codec_dev_write(s_speaker, buf, sizeof(buf));

    vTaskDelete(NULL);
}

/* ── Public API ── */

esp_err_t pico8_audio_init(void)
{
    return hw_init();
}

esp_err_t pico8_audio_start(void)
{
    if (s_active) return ESP_OK;

    esp_err_t err = hw_init();
    if (err != ESP_OK) return err;

    /* Create ring buffer */
    if (!s_ring) {
        s_ring = xStreamBufferCreate(CONFIG_P3A_PICO8_AUDIO_BUFFER_SIZE, 1);
        if (!s_ring) {
            ESP_LOGE(TAG, "Failed to create stream buffer");
            return ESP_ERR_NO_MEM;
        }
    } else {
        xStreamBufferReset(s_ring);
    }

    s_active = true;
    esp_codec_dev_set_out_mute(s_speaker, false);

    BaseType_t ok = xTaskCreate(audio_feed_task, "pico8_audio",
                                AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO,
                                &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio feed task");
        s_active = false;
        esp_codec_dev_set_out_mute(s_speaker, true);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio playback started");
    return ESP_OK;
}

esp_err_t pico8_audio_feed(const int16_t *samples, size_t num_samples)
{
    if (!s_active || !s_ring) return ESP_ERR_INVALID_STATE;
    if (!samples || num_samples == 0) return ESP_ERR_INVALID_ARG;

    size_t bytes = num_samples * sizeof(int16_t);
    /* Non-blocking send; drop if buffer full (overrun) */
    xStreamBufferSend(s_ring, samples, bytes, 0);
    return ESP_OK;
}

void pico8_audio_stop(void)
{
    if (!s_active) return;

    ESP_LOGI(TAG, "Stopping audio playback");
    s_active = false;

    /* Wait for task to finish (it checks s_active) */
    if (s_task) {
        /* Give the task time to drain and delete itself */
        vTaskDelay(pdMS_TO_TICKS(100));
        s_task = NULL;
    }

    if (s_speaker) {
        esp_codec_dev_set_out_mute(s_speaker, true);
    }

    if (s_ring) {
        xStreamBufferReset(s_ring);
    }
}

bool pico8_audio_is_active(void)
{
    return s_active;
}
