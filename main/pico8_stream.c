#include "pico8_stream.h"

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE

#define PICO8_FRAME_WIDTH        128
#define PICO8_FRAME_HEIGHT       128
#define PICO8_FRAME_BYTES        (PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT / 2)
#define PICO8_PALETTE_COLORS     16

#include <string.h>

#include "animation_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"

#define PICO8_STREAM_STACK_SIZE 4096
#define PICO8_STREAM_TASK_PRIORITY (CONFIG_P3A_RENDER_TASK_PRIORITY)
#define PICO8_STREAM_FLAG_PALETTE 0x01
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct __attribute__((packed)) {
    uint16_t payload_len;
    uint8_t flags;
} pico8_stream_header_t;

typedef enum {
    STREAM_STATE_HEADER = 0,
    STREAM_STATE_PALETTE,
    STREAM_STATE_FRAME,
} pico8_stream_state_t;

static const char *TAG = "pico8_stream";
static TaskHandle_t s_stream_task = NULL;
static pico8_stream_state_t s_state = STREAM_STATE_HEADER;
static pico8_stream_header_t s_header = {0};
static size_t s_bytes_collected = 0;
static uint8_t s_palette_data[PICO8_PALETTE_COLORS * 3];
static uint8_t s_frame_data[PICO8_FRAME_BYTES];

static void reset_state(void)
{
    s_state = STREAM_STATE_HEADER;
    s_bytes_collected = 0;
    memset(&s_header, 0, sizeof(s_header));
}

static void submit_frame(bool has_palette)
{
    const uint8_t *palette = has_palette ? s_palette_data : NULL;
    esp_err_t err = animation_player_submit_pico8_frame(palette,
                                                        has_palette ? sizeof(s_palette_data) : 0,
                                                        s_frame_data,
                                                        s_header.payload_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to submit PICO-8 frame: %s", esp_err_to_name(err));
    }
}

static void process_bytes(const uint8_t *data, size_t len)
{
    size_t idx = 0;
    while (idx < len) {
        switch (s_state) {
        case STREAM_STATE_HEADER: {
            size_t needed = sizeof(pico8_stream_header_t) - s_bytes_collected;
            size_t copy = MIN(needed, len - idx);
            memcpy(((uint8_t *)&s_header) + s_bytes_collected, data + idx, copy);
            s_bytes_collected += copy;
            idx += copy;
            if (s_bytes_collected == sizeof(pico8_stream_header_t)) {
                if (s_header.payload_len == 0 || s_header.payload_len > sizeof(s_frame_data)) {
                    ESP_LOGW(TAG, "Invalid frame length %u, resetting stream", (unsigned)s_header.payload_len);
                    reset_state();
                    break;
                }
                s_state = (s_header.flags & PICO8_STREAM_FLAG_PALETTE) ? STREAM_STATE_PALETTE : STREAM_STATE_FRAME;
                s_bytes_collected = 0;
            }
            break;
        }
        case STREAM_STATE_PALETTE: {
            size_t needed = sizeof(s_palette_data) - s_bytes_collected;
            size_t copy = MIN(needed, len - idx);
            memcpy(s_palette_data + s_bytes_collected, data + idx, copy);
            s_bytes_collected += copy;
            idx += copy;
            if (s_bytes_collected == sizeof(s_palette_data)) {
                s_state = STREAM_STATE_FRAME;
                s_bytes_collected = 0;
            }
            break;
        }
        case STREAM_STATE_FRAME: {
            size_t needed = s_header.payload_len - s_bytes_collected;
            if (needed > sizeof(s_frame_data)) {
                needed = sizeof(s_frame_data);
            }
            size_t copy = MIN(needed, len - idx);
            memcpy(s_frame_data + s_bytes_collected, data + idx, copy);
            s_bytes_collected += copy;
            idx += copy;
            if (s_bytes_collected >= s_header.payload_len) {
                submit_frame((s_header.flags & PICO8_STREAM_FLAG_PALETTE) != 0);
                reset_state();
            }
            break;
        }
        default:
            reset_state();
            break;
        }
    }
}

static void pico8_stream_task(void *arg)
{
    (void)arg;
    uint8_t chunk[512];

    while (true) {
        if (!tud_ready()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t available = tud_vendor_available();
        if (available == 0) {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }

        uint32_t to_read = (available < sizeof(chunk)) ? available : sizeof(chunk);
        int32_t received = tud_vendor_read(chunk, to_read);
        if (received > 0) {
            process_bytes(chunk, (size_t)received);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

esp_err_t pico8_stream_init(void)
{
    if (s_stream_task) {
        return ESP_OK;
    }

    reset_state();

    BaseType_t created = xTaskCreatePinnedToCore(
        pico8_stream_task,
        "pico8_stream",
        PICO8_STREAM_STACK_SIZE,
        NULL,
        PICO8_STREAM_TASK_PRIORITY,
        &s_stream_task,
        tskNO_AFFINITY);

    if (created != pdPASS) {
        s_stream_task = NULL;
        ESP_LOGE(TAG, "Failed to create PICO-8 stream task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PICO-8 stream task started");
    return ESP_OK;
}

void pico8_stream_reset(void)
{
    reset_state();
}

#else

esp_err_t pico8_stream_init(void)
{
    return ESP_OK;
}

void pico8_stream_reset(void)
{
}

#endif  // CONFIG_P3A_PICO8_USB_STREAM_ENABLE


