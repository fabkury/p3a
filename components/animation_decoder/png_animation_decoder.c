// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file png_animation_decoder.c
 * @brief PNG/APNG decoder using libpng with transparency compositing
 *
 * Static PNGs are fully decoded at init (libpng structs torn down before
 * returning, source_consumed=true). APNGs (acTL with more than one frame)
 * keep the libpng read structs alive and decode frames on demand from the
 * retained file buffer (source_consumed=false), compositing fcTL sub-frames
 * onto a persistent RGBA canvas. Requires the APNG-patched libpng fork in
 * components/espressif__libpng; without PNG_READ_APNG_SUPPORTED this file
 * degrades to static-only PNG decoding.
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "static_image_decoder_common.h"
#include "config_store.h"
#include "png.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#define TAG "png_decoder"

// PNG decoder implementation structure
typedef struct {
    const uint8_t *file_data;
    size_t file_size;
    size_t read_offset;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint8_t *rgb_buffer;   // Static path: RGB888 when opaque
    size_t rgb_buffer_size;
    uint8_t *rgba_buffer;  // Static path: RGBA8888 when source has transparency
    size_t rgba_buffer_size;
    bool has_transparency;
    bool initialized;
    uint32_t current_frame_delay_ms;

#ifdef PNG_READ_APNG_SUPPORTED
    // APNG (animated) state. The libpng read structs stay alive across
    // decode calls; reset() destroys and recreates them from file_data.
    bool is_animation;
    bool errored;            // latched on libpng longjmp; cleared by reset()
    png_structp png_ptr;
    png_infop info_ptr;
    int interlace_passes;
    uint32_t num_frames;     // animation frames (excludes hidden default image)
    uint32_t frames_emitted; // frames composited since init/reset
    bool first_frame_hidden; // IDAT default image is not part of the animation
    uint8_t *canvas;         // RGBA8888 canvas, persistent across frames
    uint8_t *subframe;       // RGBA8888 scratch for one fcTL sub-frame
    uint8_t *prev_snapshot;  // lazy canvas-rect copy for PNG_DISPOSE_OP_PREVIOUS
    // Disposal of the most recently composited frame, applied before the next
    uint8_t pending_dispose_op;
    bool have_pending_dispose;
    uint32_t pend_x, pend_y, pend_w, pend_h;
#endif
} png_decoder_data_t;

// Custom read function for libpng to read from memory buffer
static void png_read_from_memory(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_decoder_data_t *png_data = (png_decoder_data_t *)png_get_io_ptr(png_ptr);
    if (!png_data || !png_data->file_data) {
        png_error(png_ptr, "Invalid PNG data pointer");
        return;
    }

    if (png_data->read_offset + length > png_data->file_size) {
        png_error(png_ptr, "Read beyond end of PNG data");
        return;
    }

    memcpy(data, png_data->file_data + png_data->read_offset, length);
    png_data->read_offset += length;
}

// PSRAM-preferring pixel buffer allocation (matches the other decoders)
static uint8_t *alloc_pixels(size_t size)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc(size);
    }
    return buf;
}

static inline uint8_t div255_u16(uint16_t x)
{
    // Accurate (x / 255) for 0..65535 using 16-bit math.
    uint16_t t = (uint16_t)(x + 128);
    return (uint8_t)((t + (t >> 8)) >> 8);
}

static inline uint8_t blend_chan(uint8_t src, uint8_t bg, uint8_t a)
{
    const uint16_t inv = (uint16_t)(255U - (uint16_t)a);
    const uint16_t x = (uint16_t)src * (uint16_t)a + (uint16_t)bg * inv;
    return div255_u16(x);
}

// Flatten an RGBA8888 buffer into the caller's RGB888 buffer, compositing
// partially/fully transparent pixels against the configured background color.
static void flatten_rgba_to_rgb_bg(const uint8_t *src, uint8_t *dst, size_t pixel_count)
{
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    config_store_get_background_color(&bg_r, &bg_g, &bg_b);

    for (size_t i = 0; i < pixel_count; i++) {
        const uint8_t r = src[i * 4 + 0];
        const uint8_t g = src[i * 4 + 1];
        const uint8_t b = src[i * 4 + 2];
        const uint8_t a = src[i * 4 + 3];
        if (a == 255) {
            dst[i * 3 + 0] = r;
            dst[i * 3 + 1] = g;
            dst[i * 3 + 2] = b;
        } else if (a == 0) {
            dst[i * 3 + 0] = bg_r;
            dst[i * 3 + 1] = bg_g;
            dst[i * 3 + 2] = bg_b;
        } else {
            dst[i * 3 + 0] = blend_chan(r, bg_r, a);
            dst[i * 3 + 1] = blend_chan(g, bg_g, a);
            dst[i * 3 + 2] = blend_chan(b, bg_b, a);
        }
    }
}

#ifdef PNG_READ_APNG_SUPPORTED

// Non-premultiplied src-over for PNG_BLEND_OP_OVER (APNG spec formula).
static inline void blend_over_rgba(uint8_t *dst, const uint8_t *src)
{
    const uint32_t a_s = src[3];
    if (a_s == 255U) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255U;
        return;
    }
    if (a_s == 0U) {
        return;
    }
    const uint32_t a_d = dst[3];
    const uint32_t inv = 255U - a_s;
    const uint32_t denom = 255U * a_s + a_d * inv;  // == 255 * a_out
    if (denom == 0U) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0U;
        return;
    }
    for (int c = 0; c < 3; c++) {
        const uint32_t num = 255U * a_s * src[c] + a_d * inv * dst[c];
        dst[c] = (uint8_t)(num / denom);
    }
    dst[3] = (uint8_t)((denom + 127U) / 255U);
}

static void apng_destroy_read_structs(png_decoder_data_t *d)
{
    if (d->png_ptr) {
        png_destroy_read_struct(&d->png_ptr, d->info_ptr ? &d->info_ptr : NULL, NULL);
        d->png_ptr = NULL;
        d->info_ptr = NULL;
    }
}

// (Re)open the bitstream for animated decoding: fresh read structs, info
// parsed, transforms forced to RGBA8888 output. Used by init and reset.
static esp_err_t apng_open_stream(png_decoder_data_t *d)
{
    apng_destroy_read_structs(d);
    d->read_offset = 0;

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return ESP_ERR_NO_MEM;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return ESP_ERR_NO_MEM;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "APNG stream open error");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    png_set_read_fn(png_ptr, d, png_read_from_memory);
    png_read_info(png_ptr, info_ptr);

    if (!png_get_valid(png_ptr, info_ptr, PNG_INFO_acTL)) {
        ESP_LOGE(TAG, "APNG reopen: acTL chunk missing");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    const png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    const png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Force RGBA8888 rows: dispose/blend handling needs an alpha channel even
    // for alpha-less color types (DISPOSE_OP_BACKGROUND clears regions to
    // fully transparent black).
    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);

    int passes = png_set_interlace_handling(png_ptr);
    if (passes <= 0) {
        passes = 1;
    }

    png_read_update_info(png_ptr, info_ptr);

    if (png_get_rowbytes(png_ptr, info_ptr) != (size_t)d->canvas_width * 4U) {
        ESP_LOGE(TAG, "APNG: unexpected row format after transforms");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    d->png_ptr = png_ptr;
    d->info_ptr = info_ptr;
    d->interlace_passes = passes;
    d->num_frames = png_get_num_frames(png_ptr, info_ptr);
    d->first_frame_hidden = png_get_first_frame_is_hidden(png_ptr, info_ptr) != 0;
    return ESP_OK;
}

// Apply the previous frame's fcTL dispose_op to the canvas.
static void apng_apply_pending_dispose(png_decoder_data_t *d)
{
    if (!d->have_pending_dispose) {
        return;
    }
    const size_t canvas_stride = (size_t)d->canvas_width * 4U;
    const size_t rect_stride = (size_t)d->pend_w * 4U;

    if (d->pending_dispose_op == PNG_DISPOSE_OP_BACKGROUND) {
        for (uint32_t row = 0; row < d->pend_h; row++) {
            uint8_t *dst = d->canvas + (size_t)(d->pend_y + row) * canvas_stride
                         + (size_t)d->pend_x * 4U;
            memset(dst, 0, rect_stride);
        }
    } else if (d->pending_dispose_op == PNG_DISPOSE_OP_PREVIOUS && d->prev_snapshot) {
        for (uint32_t row = 0; row < d->pend_h; row++) {
            uint8_t *dst = d->canvas + (size_t)(d->pend_y + row) * canvas_stride
                         + (size_t)d->pend_x * 4U;
            memcpy(dst, d->prev_snapshot + (size_t)row * rect_stride, rect_stride);
        }
    }
    // PNG_DISPOSE_OP_NONE: leave the canvas as-is.
    d->have_pending_dispose = false;
}

// Decode one animation frame and composite it onto the canvas.
// Returns ESP_ERR_INVALID_STATE when the sequence is exhausted (the player
// resets and decodes again — same convention as animated WebP), ESP_FAIL on
// hard decode errors (latched until reset).
static esp_err_t apng_decode_one_frame(png_decoder_data_t *d)
{
    if (d->errored) {
        return ESP_FAIL;
    }
    if (d->frames_emitted >= d->num_frames) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!d->png_ptr || !d->info_ptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // Re-arm the error handler in this stack frame: the jmp_buf armed during
    // open points into a frame that no longer exists.
    if (setjmp(png_jmpbuf(d->png_ptr))) {
        ESP_LOGE(TAG, "APNG decode error at frame %u", (unsigned)d->frames_emitted);
        d->errored = true;
        return ESP_FAIL;
    }

    const size_t canvas_stride = (size_t)d->canvas_width * 4U;

    for (;;) {
        // No-op for the very first image after (re)open; otherwise advances
        // the stream past the next fcTL and re-inits row state at the
        // sub-frame dimensions.
        png_read_frame_head(d->png_ptr, d->info_ptr);

        if (!png_get_valid(d->png_ptr, d->info_ptr, PNG_INFO_fcTL)) {
            // No fcTL: this is the hidden default image (only legal as the
            // first image in the stream). Decode at full canvas size and
            // discard.
            if (d->frames_emitted != 0 || !d->first_frame_hidden) {
                png_error(d->png_ptr, "APNG image without fcTL");
            }
            for (int pass = 0; pass < d->interlace_passes; pass++) {
                for (uint32_t row = 0; row < d->canvas_height; row++) {
                    png_read_row(d->png_ptr, d->subframe + (size_t)row * canvas_stride, NULL);
                }
            }
            continue;
        }

        png_uint_32 w0 = 0, h0 = 0, x0 = 0, y0 = 0;
        png_uint_16 delay_num = 0, delay_den = 0;
        png_byte dispose_op = PNG_DISPOSE_OP_NONE, blend_op = PNG_BLEND_OP_SOURCE;
        png_get_next_frame_fcTL(d->png_ptr, d->info_ptr, &w0, &h0, &x0, &y0,
                                &delay_num, &delay_den, &dispose_op, &blend_op);

        if (w0 == 0 || h0 == 0 ||
            x0 + w0 > d->canvas_width || y0 + h0 > d->canvas_height) {
            png_error(d->png_ptr, "APNG frame rect out of bounds");
        }

        // Spec: dispose PREVIOUS on the first frame degrades to BACKGROUND.
        if (d->frames_emitted == 0 && dispose_op == PNG_DISPOSE_OP_PREVIOUS) {
            dispose_op = PNG_DISPOSE_OP_BACKGROUND;
        }

        // Dispose of the previous frame, then snapshot the region this frame
        // will cover if it asks to be reverted afterwards.
        apng_apply_pending_dispose(d);

        const size_t rect_stride = (size_t)w0 * 4U;
        if (dispose_op == PNG_DISPOSE_OP_PREVIOUS) {
            if (!d->prev_snapshot) {
                // Lazy, kept for the decoder's lifetime (canvas-sized so any
                // later frame rect fits).
                d->prev_snapshot = alloc_pixels((size_t)d->canvas_width *
                                                (size_t)d->canvas_height * 4U);
                if (!d->prev_snapshot) {
                    ESP_LOGE(TAG, "APNG: no memory for dispose-previous snapshot");
                    d->errored = true;
                    return ESP_ERR_NO_MEM;
                }
            }
            for (uint32_t row = 0; row < h0; row++) {
                const uint8_t *src = d->canvas + (size_t)(y0 + row) * canvas_stride
                                   + (size_t)x0 * 4U;
                memcpy(d->prev_snapshot + (size_t)row * rect_stride, src, rect_stride);
            }
        }

        // Decode the sub-frame rows (libpng row state is at w0×h0 here).
        for (int pass = 0; pass < d->interlace_passes; pass++) {
            for (uint32_t row = 0; row < h0; row++) {
                png_read_row(d->png_ptr, d->subframe + (size_t)row * rect_stride, NULL);
            }
        }

        // Composite onto the canvas.
        if (blend_op == PNG_BLEND_OP_SOURCE) {
            for (uint32_t row = 0; row < h0; row++) {
                uint8_t *dst = d->canvas + (size_t)(y0 + row) * canvas_stride
                             + (size_t)x0 * 4U;
                memcpy(dst, d->subframe + (size_t)row * rect_stride, rect_stride);
            }
        } else {  // PNG_BLEND_OP_OVER
            for (uint32_t row = 0; row < h0; row++) {
                uint8_t *dst = d->canvas + (size_t)(y0 + row) * canvas_stride
                             + (size_t)x0 * 4U;
                const uint8_t *src = d->subframe + (size_t)row * rect_stride;
                for (uint32_t col = 0; col < w0; col++) {
                    blend_over_rgba(dst + (size_t)col * 4U, src + (size_t)col * 4U);
                }
            }
        }

        d->pending_dispose_op = dispose_op;
        d->have_pending_dispose = true;
        d->pend_x = x0;
        d->pend_y = y0;
        d->pend_w = w0;
        d->pend_h = h0;

        const uint32_t den = (delay_den == 0) ? 100U : (uint32_t)delay_den;
        uint32_t delay_ms = ((uint32_t)delay_num * 1000U) / den;
        if (delay_ms < 1) {
            delay_ms = 1;  // Same minimum clamp as the GIF/WebP decoders
        }
        d->current_frame_delay_ms = delay_ms;

        d->frames_emitted++;
        return ESP_OK;
    }
}

// Set up animated decoding at init time: persistent canvas + scratch buffers,
// then a fresh stream open (the probe structs were torn down by the caller).
static esp_err_t apng_init_animation(png_decoder_data_t *d)
{
    const size_t canvas_size = (size_t)d->canvas_width * (size_t)d->canvas_height * 4U;

    d->canvas = alloc_pixels(canvas_size);
    d->subframe = alloc_pixels(canvas_size);
    if (!d->canvas || !d->subframe) {
        ESP_LOGE(TAG, "Failed to allocate APNG canvas (%zu bytes x2)", canvas_size);
        return ESP_ERR_NO_MEM;
    }
    memset(d->canvas, 0, canvas_size);

    esp_err_t err = apng_open_stream(d);
    if (err != ESP_OK) {
        return err;
    }

    d->is_animation = true;
    d->has_transparency = true;  // Dispose/blend can introduce transparency
    d->current_frame_delay_ms = 1;
    return ESP_OK;
}

#endif /* PNG_READ_APNG_SUPPORTED */

esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Verify PNG signature
    if (size < 8 || png_sig_cmp((png_const_bytep)data, 0, 8) != 0) {
        ESP_LOGE(TAG, "Invalid PNG signature");
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)calloc(1, sizeof(png_decoder_data_t));
    if (!png_data) {
        ESP_LOGE(TAG, "Failed to allocate PNG decoder data");
        return ESP_ERR_NO_MEM;
    }

    png_data->file_data = data;
    png_data->file_size = size;
    png_data->read_offset = 0;
    png_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    // Create PNG read structure
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read structure");
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Create PNG info structure
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info structure");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Set error handling
    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decoding error");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_FAIL;
    }

    // Set custom read function
    png_set_read_fn(png_ptr, png_data, png_read_from_memory);

    // Read PNG info
    png_read_info(png_ptr, info_ptr);

    // Get image dimensions
    png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int number_of_passes = 1;

    if (width == 0 || height == 0) {
        ESP_LOGE(TAG, "Invalid PNG dimensions: %u x %u", (unsigned)width, (unsigned)height);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_ERR_INVALID_SIZE;
    }

    png_data->canvas_width = width;
    png_data->canvas_height = height;
    png_data->has_transparency = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS);

#ifdef PNG_READ_APNG_SUPPORTED
    // APNG with more than one frame takes the animated path: tear down the
    // probe structs and reopen with animation transforms. Single-frame acTL
    // falls through to the static path below.
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_acTL) &&
        png_get_num_frames(png_ptr, info_ptr) > 1) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

        esp_err_t err = apng_init_animation(png_data);
        if (err != ESP_OK) {
            free(png_data->canvas);
            free(png_data->subframe);
            free(png_data);
            return err;
        }

        animation_decoder_t *adec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
        if (!adec) {
            ESP_LOGE(TAG, "Failed to allocate decoder");
            apng_destroy_read_structs(png_data);
            free(png_data->canvas);
            free(png_data->subframe);
            free(png_data);
            return ESP_ERR_NO_MEM;
        }

        png_data->initialized = true;
        adec->type = ANIMATION_DECODER_TYPE_PNG;
        adec->impl.png.png_decoder = png_data;
        *decoder = adec;

        ESP_LOGI(TAG, "APNG decoder initialized: %ux%u, %u frames%s",
                 (unsigned)png_data->canvas_width,
                 (unsigned)png_data->canvas_height,
                 (unsigned)png_data->num_frames,
                 png_data->first_frame_hidden ? ", hidden first frame" : "");
        return ESP_OK;
    }
#endif /* PNG_READ_APNG_SUPPORTED */

    // Transform to RGB or RGBA
    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (png_data->has_transparency) {
        // Ensure an explicit alpha channel for blending
        if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png_ptr);
        }
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
            png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
        }
    } else {
        // Keep it RGB (no alpha) for performance
        // If there is an alpha channel, has_transparency would already be true.
    }

    number_of_passes = png_set_interlace_handling(png_ptr);
    if (number_of_passes <= 0) {
        number_of_passes = 1;
    }

    // Update info after transformations
    png_read_update_info(png_ptr, info_ptr);

    const png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    if (png_data->has_transparency) {
        png_data->rgba_buffer_size = (size_t)rowbytes * (size_t)height;
        png_data->rgba_buffer = alloc_pixels(png_data->rgba_buffer_size);
        if (!png_data->rgba_buffer) {
            ESP_LOGE(TAG, "Failed to allocate RGBA buffer (%zu bytes)", png_data->rgba_buffer_size);
            png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
            free(png_data);
            return ESP_ERR_NO_MEM;
        }
    } else {
        png_data->rgb_buffer_size = (size_t)rowbytes * (size_t)height;
        png_data->rgb_buffer = alloc_pixels(png_data->rgb_buffer_size);
        if (!png_data->rgb_buffer) {
            ESP_LOGE(TAG, "Failed to allocate RGB buffer (%zu bytes)", png_data->rgb_buffer_size);
            png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
            free(png_data);
            return ESP_ERR_NO_MEM;
        }
    }

    // Allocate row pointers
    png_bytep *row_pointers = (png_bytep *)heap_caps_malloc(height * sizeof(png_bytep),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row_pointers) {
        row_pointers = (png_bytep *)heap_caps_malloc(height * sizeof(png_bytep), MALLOC_CAP_8BIT);
    }
    if (!row_pointers) {
        ESP_LOGE(TAG, "Failed to allocate row pointers");
        free(png_data->rgba_buffer);
        free(png_data->rgb_buffer);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    // Set up row pointers
    uint8_t *base = png_data->has_transparency ? png_data->rgba_buffer : png_data->rgb_buffer;
    for (png_uint_32 y = 0; y < height; y++) {
        row_pointers[y] = base + (size_t)y * (size_t)rowbytes;
    }

    // Read image data (handle interlaced images if needed)
    if (number_of_passes > 1) {
        for (int pass = 0; pass < number_of_passes; pass++) {
            for (png_uint_32 y = 0; y < height; y++) {
                png_bytep row = row_pointers[y];
                png_read_row(png_ptr, row, NULL);
            }
        }
    } else {
        png_read_image(png_ptr, row_pointers);
    }
    png_read_end(png_ptr, NULL);

    // Clean up libpng structures
    free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    png_data->initialized = true;

    // Create decoder structure
    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        free(png_data->rgba_buffer);
        free(png_data->rgb_buffer);
        free(png_data);
        return ESP_ERR_NO_MEM;
    }

    dec->type = ANIMATION_DECODER_TYPE_PNG;
    dec->impl.png.png_decoder = png_data;

    *decoder = dec;

    ESP_LOGI(TAG, "PNG decoder initialized: %ux%u, transparency=%d",
             (unsigned)png_data->canvas_width,
             (unsigned)png_data->canvas_height,
             png_data->has_transparency);

    return ESP_OK;
}

esp_err_t png_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    if (!decoder || !info || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    info->canvas_width = png_data->canvas_width;
    info->canvas_height = png_data->canvas_height;
    info->has_transparency = png_data->has_transparency;
    info->pixel_format = ANIMATION_PIXEL_FORMAT_RGB888;

#ifdef PNG_READ_APNG_SUPPORTED
    if (png_data->is_animation) {
        info->frame_count = png_data->num_frames;
        // The decoder reads frames on demand from file_data for its whole
        // lifetime — the loader must keep the buffer alive.
        info->source_consumed = false;
        return ESP_OK;
    }
#endif

    info->frame_count = 1; // Static PNG is always single frame
    // libpng reads the entire bitstream synchronously inside png_decoder_init
    // (png_read_image + png_destroy_read_struct); nothing references
    // file_data afterwards.
    info->source_consumed = true;

    return ESP_OK;
}

esp_err_t png_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#ifdef PNG_READ_APNG_SUPPORTED
    if (png_data->is_animation) {
        esp_err_t err = apng_decode_one_frame(png_data);
        if (err != ESP_OK) {
            return err;
        }
        memcpy(rgba_buffer, png_data->canvas,
               (size_t)png_data->canvas_width * (size_t)png_data->canvas_height * 4U);
        return ESP_OK;
    }
#endif

    if (png_data->has_transparency) {
        if (!png_data->rgba_buffer || png_data->rgba_buffer_size == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(rgba_buffer, png_data->rgba_buffer, png_data->rgba_buffer_size);
    } else {
        if (!png_data->rgb_buffer || png_data->rgb_buffer_size == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t pixel_count = (size_t)png_data->canvas_width * (size_t)png_data->canvas_height;
        const uint8_t *src = png_data->rgb_buffer;
        for (size_t i = 0; i < pixel_count; i++) {
            rgba_buffer[i * 4 + 0] = src[i * 3 + 0];
            rgba_buffer[i * 4 + 1] = src[i * 3 + 1];
            rgba_buffer[i * 4 + 2] = src[i * 3 + 2];
            rgba_buffer[i * 4 + 3] = 255;
        }
        // Opaque branch: drop the intermediate. See decode_next_rgb for rationale.
        free(png_data->rgb_buffer);
        png_data->rgb_buffer = NULL;
        png_data->rgb_buffer_size = 0;
    }
    png_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t png_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    if (!decoder || !rgb_buffer || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#ifdef PNG_READ_APNG_SUPPORTED
    if (png_data->is_animation) {
        esp_err_t err = apng_decode_one_frame(png_data);
        if (err != ESP_OK) {
            return err;
        }
        // Flatten per frame against the current background color (free
        // mid-playback background tracking, like animated WebP).
        flatten_rgba_to_rgb_bg(png_data->canvas, rgb_buffer,
                               (size_t)png_data->canvas_width * (size_t)png_data->canvas_height);
        return ESP_OK;
    }
#endif

    if (!png_data->has_transparency) {
        if (!png_data->rgb_buffer || png_data->rgb_buffer_size == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t sz = (size_t)png_data->canvas_width * (size_t)png_data->canvas_height * 3U;
        memcpy(rgb_buffer, png_data->rgb_buffer, sz);
        png_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
        // Opaque PNG never re-decodes (no bg-recompose path), so the
        // intermediate buffer is dead weight after the caller's b1 holds
        // the pixels. Free it to halve the per-asset PSRAM footprint.
        // For transparent PNGs we keep rgba_buffer alive because the
        // renderer's static fast path re-invokes decode_next_rgb on
        // background-color change to recomposite against the new bg.
        free(png_data->rgb_buffer);
        png_data->rgb_buffer = NULL;
        png_data->rgb_buffer_size = 0;
        return ESP_OK;
    }

    if (!png_data->rgba_buffer || png_data->rgba_buffer_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    flatten_rgba_to_rgb_bg(png_data->rgba_buffer, rgb_buffer,
                           (size_t)png_data->canvas_width * (size_t)png_data->canvas_height);

    png_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
    return ESP_OK;
}

esp_err_t png_decoder_reset(animation_decoder_t *decoder)
{
    if (!decoder || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#ifdef PNG_READ_APNG_SUPPORTED
    if (png_data->is_animation) {
        // Recreate the libpng read structs from the retained file buffer;
        // pixel buffers (canvas/subframe/snapshot) are kept to avoid
        // per-loop allocation churn.
        memset(png_data->canvas, 0,
               (size_t)png_data->canvas_width * (size_t)png_data->canvas_height * 4U);
        png_data->frames_emitted = 0;
        png_data->have_pending_dispose = false;
        png_data->current_frame_delay_ms = 1;

        esp_err_t err = apng_open_stream(png_data);
        if (err != ESP_OK) {
            png_data->errored = true;
            return ESP_FAIL;
        }
        png_data->errored = false;
        return ESP_OK;
    }
#endif

    // PNG is static, so reset just restores the delay
    png_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t png_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    if (!decoder || !delay_ms || decoder->type != ANIMATION_DECODER_TYPE_PNG) {
        return ESP_ERR_INVALID_ARG;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)decoder->impl.png.png_decoder;
    if (!png_data || !png_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *delay_ms = png_data->current_frame_delay_ms;
    return ESP_OK;
}

void png_decoder_unload(animation_decoder_t **decoder)
{
    if (!decoder || !*decoder) {
        return;
    }

    animation_decoder_t *dec = *decoder;
    if (dec->type != ANIMATION_DECODER_TYPE_PNG) {
        return;
    }

    png_decoder_data_t *png_data = (png_decoder_data_t *)dec->impl.png.png_decoder;
    if (png_data) {
#ifdef PNG_READ_APNG_SUPPORTED
        apng_destroy_read_structs(png_data);
        if (png_data->canvas) {
            free(png_data->canvas);
            png_data->canvas = NULL;
        }
        if (png_data->subframe) {
            free(png_data->subframe);
            png_data->subframe = NULL;
        }
        if (png_data->prev_snapshot) {
            free(png_data->prev_snapshot);
            png_data->prev_snapshot = NULL;
        }
#endif
        if (png_data->rgb_buffer) {
            free(png_data->rgb_buffer);
            png_data->rgb_buffer = NULL;
        }
        if (png_data->rgba_buffer) {
            free(png_data->rgba_buffer);
            png_data->rgba_buffer = NULL;
        }
        free(png_data);
    }

    free(dec);
    *decoder = NULL;
}
