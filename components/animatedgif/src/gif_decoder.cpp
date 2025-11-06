// This file implements C++ code, so ensure C++ linkage
#ifdef __cplusplus

// Include C++ header first, before any extern "C" blocks
#include "AnimatedGIF.h"
#include "animatedgif_compat.h"

// Now include C header (which has extern "C")
#include "gif_decoder.h"

#else
// If compiled as C, include C header only
#include "gif_decoder.h"
#endif

#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <algorithm>

#ifdef __cplusplus
// All C++ code follows

static const char *TAG = "gif_decoder";

static inline void rgb565_to_bgr888(uint16_t pixel, uint8_t *out)
{
    // RGB565 layout: RRRRRGGGGGGBBBBB (MSB->LSB)
    // Convert to panel-friendly BGR888 byte order
    uint8_t r = (uint8_t)(((pixel >> 11) & 0x1F) << 3);
    r |= (r >> 5);  // replicate LSBs to improve fidelity

    uint8_t g = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
    g |= (g >> 6);

    uint8_t b = (uint8_t)((pixel & 0x1F) << 3);
    b |= (b >> 5);

    out[0] = b;
    out[1] = g;
    out[2] = r;
}

// File I/O callbacks for AnimatedGIF
static void *gifOpenFileCallback(const char *fname, int32_t *pFileSize)
{
    FILE *f = fopen(fname, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open GIF file: %s", fname);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Failed to seek GIF file: %s", fname);
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0) {
        ESP_LOGE(TAG, "Failed to determine GIF file size: %s", fname);
        fclose(f);
        return NULL;
    }

    if (size > INT32_MAX) {
        ESP_LOGE(TAG, "GIF file too large: %s (%ld)", fname, size);
        fclose(f);
        return NULL;
    }

    *pFileSize = (int32_t)size;
    fseek(f, 0, SEEK_SET);
    return f;
}

static void gifCloseFileCallback(void *handle)
{
    if (handle) {
        fclose((FILE *)handle);
    }
}

static int32_t gifReadFileCallback(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    if (!pFile || !pFile->fHandle) {
        return 0;
    }

    FILE *f = (FILE *)pFile->fHandle;
    size_t read = fread(pBuf, 1, (size_t)iLen, f);
    pFile->iPos += (int32_t)read;
    return (int32_t)read;
}

static int32_t gifSeekFileCallback(GIFFILE *pFile, int32_t iPosition)
{
    if (!pFile || !pFile->fHandle) {
        return 0;
    }

    FILE *f = (FILE *)pFile->fHandle;
    if (fseek(f, iPosition, SEEK_SET) == 0) {
        pFile->iPos = iPosition;
        return 1;
    }
    return 0;
}

// GIFDraw callback - converts scanlines to RGB888
// This is called for each scanline of the GIF frame
static void GIFDraw(GIFDRAW *pDraw)
{
    gif_draw_context_t *ctx = (gif_draw_context_t *)pDraw->pUser;
    if (!ctx) {
        return;
    }

    int y = pDraw->y;
    int src_w = pDraw->iWidth;
    int canvas_w = pDraw->iCanvasWidth;

    // Calculate destination position accounting for frame offset
    int dst_y = pDraw->iY + y;

    // Handle frame buffer for disposal methods
    if (ctx->frame_buffer && dst_y < ctx->frame_height) {
        uint8_t *frame_row = ctx->frame_buffer + (size_t)dst_y * canvas_w * 3;
        uint8_t *pixels = pDraw->pPixels;

        for (int x = 0; x < src_w; x++) {
            int dst_x = pDraw->iX + x;
            if (dst_x < 0 || dst_x >= canvas_w) {
                continue;
            }

            uint8_t pixel_idx = pixels[x];
            if (pDraw->ucHasTransparency && pixel_idx == pDraw->ucTransparent) {
                continue;
            }

            uint8_t *dst_px = frame_row + (size_t)dst_x * 3;

            if (pDraw->ucPaletteType == GIF_PALETTE_RGB565_LE) {
                uint16_t pixel = pDraw->pPalette[pixel_idx];
                rgb565_to_bgr888(pixel, dst_px);
            } else if (pDraw->ucPaletteType == GIF_PALETTE_RGB888) {
                uint8_t *palette24 = pDraw->pPalette24;
                dst_px[0] = palette24[pixel_idx * 3 + 2];  // B
                dst_px[1] = palette24[pixel_idx * 3 + 1];  // G
                dst_px[2] = palette24[pixel_idx * 3 + 0];  // R
            }
        }
    }

    // If stripe buffer is set, also write to stripe (for immediate DMA)
    if (ctx->stripe_buffer && dst_y >= ctx->stripe_y && dst_y < ctx->stripe_y + ctx->stripe_height) {
        int stripe_line = dst_y - ctx->stripe_y;
        uint8_t *dst_row = ctx->stripe_buffer + (size_t)stripe_line * ctx->display_width * 3;

        if (ctx->frame_buffer) {
            // Copy from frame buffer, but don't read past the frame width
            uint8_t *frame_row = ctx->frame_buffer + (size_t)dst_y * canvas_w * 3;
            size_t copy_width = static_cast<size_t>(std::min(ctx->frame_width, ctx->display_width));
            size_t copy_bytes = copy_width * 3;
            memcpy(dst_row, frame_row, copy_bytes);

                if (copy_width < ctx->display_width) {
                size_t pad_px = ctx->display_width - copy_width;
                memset(dst_row + copy_bytes, 0, pad_px * 3);
                if (stripe_line == 0) {
                        ESP_LOGI(TAG, "GIF stripe padding applied: dst_y=%d copy_width=%zu pad_px=%zu", dst_y, copy_width, pad_px);
                }
            }
        } else {
            uint8_t *pixels = pDraw->pPixels;
            for (int x = 0; x < src_w && x < ctx->display_width; x++) {
                uint8_t pixel_idx = pixels[x];
                if (pDraw->ucHasTransparency && pixel_idx == pDraw->ucTransparent) {
                    continue;
                }

                uint8_t *dst_px = dst_row + (size_t)x * 3;

                if (pDraw->ucPaletteType == GIF_PALETTE_RGB565_LE) {
                    uint16_t pixel = pDraw->pPalette[pixel_idx];
                    rgb565_to_bgr888(pixel, dst_px);
                } else if (pDraw->ucPaletteType == GIF_PALETTE_RGB888) {
                    uint8_t *palette24 = pDraw->pPalette24;
                    dst_px[0] = palette24[pixel_idx * 3 + 2];  // B
                    dst_px[1] = palette24[pixel_idx * 3 + 1];  // G
                    dst_px[2] = palette24[pixel_idx * 3 + 0];  // R
                }
            }
        }
    }
}

esp_err_t gif_decoder_init(gif_decoder_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(state, 0, sizeof(gif_decoder_state_t));
    state->gif = (void *)new AnimatedGIF();
    if (!state->gif) {
        return ESP_ERR_NO_MEM;
    }
    
    AnimatedGIF *gif = (AnimatedGIF *)state->gif;
    gif->begin(GIF_PALETTE_RGB888);
    gif->setDrawType(GIF_DRAW_RAW);  // Use RAW mode for scanline-by-scanline callback
    
    return ESP_OK;
}

esp_err_t gif_decoder_open_file(gif_decoder_state_t *state, const char *file_path)
{
    if (!state || !file_path || !state->gif) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (state->file_path) {
        free(state->file_path);
        state->file_path = NULL;
    }

    if (state->memory_data) {
        free(state->memory_data);
        state->memory_data = NULL;
        state->memory_size = 0;
    }

    state->file_handle = NULL;

    if (state->memory_data) {
        free(state->memory_data);
        state->memory_data = NULL;
        state->memory_size = 0;
    }

    state->file_handle = NULL;

    state->file_path = strdup(file_path);
    if (!state->file_path) {
        return ESP_ERR_NO_MEM;
    }
    state->memory_data = NULL;
    state->memory_size = 0;
    state->file_handle = NULL;
    
    AnimatedGIF *gif = (AnimatedGIF *)state->gif;
    int result = gif->open(
        file_path,
        gifOpenFileCallback,
        gifCloseFileCallback,
        gifReadFileCallback,
        gifSeekFileCallback,
        GIFDraw  // Pass the callback function
    );
    
    if (result == 0) {
        ESP_LOGE(TAG, "Failed to open GIF file: %s", file_path);
        free(state->file_path);
        state->file_path = NULL;
        return ESP_FAIL;
    }
    
    state->canvas_width = gif->getCanvasWidth();
    state->canvas_height = gif->getCanvasHeight();
    state->loop_count = gif->getLoopCount();
    
    ESP_LOGI(TAG, "Opened GIF: %s (%dx%d, loops=%d)", 
             file_path, state->canvas_width, state->canvas_height, state->loop_count);
    
    return ESP_OK;
}

esp_err_t gif_decoder_open_memory(gif_decoder_state_t *state, const uint8_t *data, size_t size)
{
    if (!state || !data || size == 0 || !state->gif) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Free any previous memory-based data
    if (state->memory_data) {
        free(state->memory_data);
        state->memory_data = NULL;
        state->memory_size = 0;
    }

    // Allocate copy for AnimatedGIF (it needs to modify the buffer)
    uint8_t *data_copy = (uint8_t *)malloc(size);
    if (!data_copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(data_copy, data, size);
    
    AnimatedGIF *gif = (AnimatedGIF *)state->gif;
    int result = gif->open(data_copy, (int)size, GIFDraw);
    
    if (result == 0) {
        ESP_LOGE(TAG, "Failed to open GIF from memory");
        free(data_copy);
        return ESP_FAIL;
    }
    state->memory_data = data_copy;
    state->memory_size = size;
    state->file_path = NULL;
    
    state->canvas_width = gif->getCanvasWidth();
    state->canvas_height = gif->getCanvasHeight();
    state->loop_count = gif->getLoopCount();
    
    ESP_LOGI(TAG, "Opened GIF from memory (%dx%d, loops=%d)", 
             state->canvas_width, state->canvas_height, state->loop_count);
    
    return ESP_OK;
}

bool gif_decoder_play_frame(gif_decoder_state_t *state, int *delay_ms_out)
{
    if (!state || !state->gif) {
        return false;
    }
    
    AnimatedGIF *gif = (AnimatedGIF *)state->gif;
    int delay_ms = 0;
    int result = gif->playFrame(false, &delay_ms, state->user_data);
    
    if (delay_ms_out) {
        *delay_ms_out = delay_ms;
    }
    state->current_frame_delay_ms = delay_ms;
    
    if (result == 0) {
        // End of animation
        if (state->should_loop || state->loop_count == 0) {
            gif->reset();
            return gif_decoder_play_frame(state, delay_ms_out);
        }
        return false;
    }
    
    return (result > 0);
}

// Set draw context for stripe-based rendering
void gif_decoder_set_draw_context(gif_decoder_state_t *state, gif_draw_context_t *draw_context)
{
    if (state) {
        state->user_data = draw_context;
    }
}

void gif_decoder_reset(gif_decoder_state_t *state)
{
    if (state && state->gif) {
        AnimatedGIF *gif = (AnimatedGIF *)state->gif;
        gif->reset();
    }
}

void gif_decoder_close(gif_decoder_state_t *state)
{
    if (!state) {
        return;
    }
    
    if (state->gif) {
        AnimatedGIF *gif = (AnimatedGIF *)state->gif;
        gif->close();
        delete gif;
        state->gif = NULL;
    }
    
    if (state->file_path) {
        free(state->file_path);
        state->file_path = NULL;
    }
}

void gif_decoder_get_canvas_size(gif_decoder_state_t *state, int *width_out, int *height_out)
{
    if (state && width_out && height_out) {
        *width_out = state->canvas_width;
        *height_out = state->canvas_height;
    }
}

int gif_decoder_get_loop_count(gif_decoder_state_t *state)
{
    if (state && state->gif) {
        AnimatedGIF *gif = (AnimatedGIF *)state->gif;
        return gif->getLoopCount();
    }
    return 0;
}

void gif_decoder_set_loop(gif_decoder_state_t *state, bool loop)
{
    if (state) {
        state->should_loop = loop;
    }
}

#endif // __cplusplus
