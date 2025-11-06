#pragma once

#include "player.h"
#include "scaler_nn.h"
#include "gif_decoder.h"
#include "webp/demux.h"
#include "esp_lcd_panel_ops.h"
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Player context structure
typedef struct player_ctx {
    // Buffers
    uint8_t* native_buf1;  // PSRAM, double-buffered
    uint8_t* native_buf2;  // PSRAM, double-buffered
    uint8_t* strip_buf1;   // SRAM, ping-pong
    uint8_t* strip_buf2;   // SRAM, ping-pong
    
    // Current state
    uint8_t* nwrite;       // Current write buffer (decoder writes here)
    uint8_t* nread;        // Current read buffer (renderer reads here)
    uint8_t* strip_ping;   // Current ping strip buffer
    uint8_t* strip_pong;   // Current pong strip buffer
    
    // Animation info
    anim_desc_t current_desc;
    int native_width;
    int native_height;
    bool running;
    
    // Decoder state
    union {
        gif_decoder_state_t* gif_decoder;
        WebPAnimDecoder* webp_decoder;
    } decoder;
    bool is_gif;
    uint8_t* webp_data_buffer;  // Track WebP data for cleanup
    
    // Panel/DMA
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t trans_sem;
    SemaphoreHandle_t vsync_sem;
    
    // Tasks and queues
    TaskHandle_t decoder_task;
    TaskHandle_t renderer_task;
    QueueHandle_t dec2ren_q;  // Decoder -> Renderer (frame ready)
    QueueHandle_t ren2dec_q;   // Renderer -> Decoder (back-pressure)
    
    // Scaler map
    const nn_map_t* scaler_map;
} player_ctx_t;

// Internal API for decoder/renderer tasks
player_ctx_t* player_get_ctx(void);

#ifdef __cplusplus
}
#endif

