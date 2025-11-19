#include "pico8_stream.h"

#define PICO8_FRAME_WIDTH        128
#define PICO8_FRAME_HEIGHT       128
#define PICO8_FRAME_BYTES        (PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT / 2)
#define PICO8_PALETTE_COLORS     16

#include <string.h>
#include <stdbool.h>

#include "animation_player.h"
#include "app_lcd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define PICO8_STREAM_FLAG_PALETTE 0x01
#define PICO8_MODE_TIMEOUT_MS 30000  // 30 seconds

static const char *TAG = "pico8_stream";
static SemaphoreHandle_t s_stream_mutex = NULL;
static uint8_t s_palette_data[PICO8_PALETTE_COLORS * 3];
static uint8_t s_frame_data[PICO8_FRAME_BYTES];
static bool s_pico8_mode_active = false;
static esp_timer_handle_t s_timeout_timer = NULL;
static int64_t s_last_frame_time_us = 0;

static void timeout_timer_callback(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "PICO-8 mode timeout, exiting mode");
    pico8_stream_exit_mode();
}

static void submit_frame(bool has_palette, size_t frame_len)
{
    const uint8_t *palette = has_palette ? s_palette_data : NULL;
    esp_err_t err = animation_player_submit_pico8_frame(palette,
                                                        has_palette ? sizeof(s_palette_data) : 0,
                                                        s_frame_data,
                                                        frame_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to submit PICO-8 frame: %s", esp_err_to_name(err));
    } else {
        // Reset timeout timer on successful frame submission
        s_last_frame_time_us = esp_timer_get_time();
        if (s_timeout_timer) {
            esp_timer_stop(s_timeout_timer);
            esp_timer_start_once(s_timeout_timer, PICO8_MODE_TIMEOUT_MS * 1000);
        }
    }
}

esp_err_t pico8_stream_init(void)
{
    if (s_stream_mutex) {
        return ESP_OK; // Already initialized
    }

    // Create mutex for thread-safe access
    s_stream_mutex = xSemaphoreCreateMutex();
    if (!s_stream_mutex) {
        ESP_LOGE(TAG, "Failed to create stream mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create timeout timer during initialization (not on-demand to avoid stack issues)
    if (!s_timeout_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &timeout_timer_callback,
            .name = "pico8_timeout"
        };
        esp_err_t err = esp_timer_create(&timer_args, &s_timeout_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create timeout timer: %s", esp_err_to_name(err));
            // Continue anyway, timeout just won't work
            s_timeout_timer = NULL;
        }
    }

    ESP_LOGI(TAG, "PICO-8 stream parser initialized (WebSocket only)");

    return ESP_OK;
}

void pico8_stream_reset(void)
{
    // No state to reset for packet-based parsing
}

esp_err_t pico8_stream_feed_packet(const uint8_t *packet, size_t len)
{
    if (!packet || len < 3) {
        ESP_LOGW(TAG, "Invalid packet args: packet=%p len=%zu", packet, len);
        return ESP_ERR_INVALID_ARG;
    }

    // Read magic + header: [magic:3][payload_len:2][flags:1]
    // Note: HTTP handler already validates magic bytes, but we double-check here for safety
    if (packet[0] != 0x70 || packet[1] != 0x38 || packet[2] != 0x46) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t payload_len = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8);
    uint8_t flags = packet[5];

    size_t expected_total = 6 + payload_len;
    if (len != expected_total) {
        ESP_LOGW(TAG, "Packet size mismatch: payload=%u (0x%04x) total=%zu expected=%zu flags=0x%02x",
                 (unsigned)payload_len, (unsigned)payload_len, len, expected_total, flags);
        return ESP_ERR_INVALID_SIZE;
    }

    // Validate payload length
    size_t palette_len = (flags & PICO8_STREAM_FLAG_PALETTE) ? sizeof(s_palette_data) : 0;
    size_t expected_payload = palette_len + PICO8_FRAME_BYTES;
    if (payload_len != expected_payload) {
        ESP_LOGW(TAG, "Invalid payload len %u (expected %zu) flags=0x%02x",
                 (unsigned)payload_len, expected_payload, flags);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *palette_ptr = packet + 6;
    const uint8_t *frame_ptr = palette_ptr + palette_len;

    if (!s_stream_mutex) {
        esp_err_t init_ret = pico8_stream_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    if (xSemaphoreTake(s_stream_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Copy palette and frame data
    if (palette_len > 0) {
        memcpy(s_palette_data, palette_ptr, palette_len);
    }
    memcpy(s_frame_data, frame_ptr, PICO8_FRAME_BYTES);

    // Submit frame
    submit_frame(palette_len > 0, PICO8_FRAME_BYTES);

    xSemaphoreGive(s_stream_mutex);
    return ESP_OK;
}

void pico8_stream_enter_mode(void)
{
    if (s_pico8_mode_active) {
        // Already in mode, just reset timeout
        if (s_timeout_timer) {
            esp_timer_stop(s_timeout_timer);
            esp_timer_start_once(s_timeout_timer, PICO8_MODE_TIMEOUT_MS * 1000);
        }
        return;
    }

    ESP_LOGI(TAG, "Entering PICO-8 mode");
    s_pico8_mode_active = true;
    s_last_frame_time_us = esp_timer_get_time();

    // Pause animation playback
    app_lcd_set_animation_paused(true);

    // Start timeout timer (timer should already be created during init)
    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
        esp_timer_start_once(s_timeout_timer, PICO8_MODE_TIMEOUT_MS * 1000);
    } else {
        ESP_LOGW(TAG, "Timeout timer not available");
    }
}

void pico8_stream_exit_mode(void)
{
    if (!s_pico8_mode_active) {
        return;
    }

    ESP_LOGI(TAG, "Exiting PICO-8 mode");
    s_pico8_mode_active = false;

    // Stop timeout timer
    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
    }

    // Resume animation playback
    app_lcd_set_animation_paused(false);
}

bool pico8_stream_is_active(void)
{
    return s_pico8_mode_active;
}
