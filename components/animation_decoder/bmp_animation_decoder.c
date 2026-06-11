// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file bmp_animation_decoder.c
 * @brief BMP decoder (static single-frame), hand-written, no external library
 *
 * Coverage: 1/4/8-bit palette, 16/24/32-bit truecolor, RLE4/RLE8 compression,
 * BI_BITFIELDS channel masks, bottom-up and top-down row order, and both the
 * 12-byte BITMAPCOREHEADER and the 40+ byte BITMAPINFOHEADER family
 * (40/52/56/64/108/124). The whole file is decoded synchronously in init
 * (source_consumed=true), like the other static-image decoders.
 *
 * Alpha policy: an alpha channel is honored only when the file's compression
 * is BI_BITFIELDS/BI_ALPHABITFIELDS with a nonzero alpha mask (V3+/V4/V5
 * headers). Classic BI_RGB 32-bit files render opaque - their fourth byte is
 * "reserved" per spec and is garbage in the wild; treating it as alpha would
 * blank out most real files.
 */

#include "animation_decoder.h"
#include "animation_decoder_internal.h"
#include "static_image_decoder_common.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#define TAG "bmp_decoder"

// biCompression values (Windows). OS/2 v2 headers (size 64) reuse 0..2 with
// the same meaning but assign 3/4 to Huffman-1D/RLE24, so >=3 is rejected for
// that header size.
#define BMP_BI_RGB            0u
#define BMP_BI_RLE8           1u
#define BMP_BI_RLE4           2u
#define BMP_BI_BITFIELDS      3u
#define BMP_BI_JPEG           4u
#define BMP_BI_PNG            5u
#define BMP_BI_ALPHABITFIELDS 6u

// Dimension sanity cap. Keeps every downstream size product comfortably
// inside 32-bit size_t; anything plausible-but-huge under the cap is handled
// by graceful pixel-buffer allocation failure (same philosophy as PNG).
#define BMP_MAX_DIM 16384u

// One color channel extracted from a 16/32-bit pixel word.
typedef struct {
    uint32_t shift;
    uint32_t bits;
    uint32_t max; // (1 << bits) - 1
} bmp_chan_t;

// Everything parsed/validated out of the headers; freed before init returns.
typedef struct {
    uint32_t width;
    uint32_t height;
    bool top_down;
    uint32_t bitcount;
    uint32_t compression;
    bool has_alpha;
    bmp_chan_t r, g, b, a; // valid for bitcount 16/32 only
    uint32_t pal_count;    // nonzero iff bitcount <= 8
    uint8_t pal[256][3];   // RGB
    const uint8_t *pixels; // start of pixel array
    size_t pixels_size;    // bytes from bfOffBits to EOF
} bmp_parse_t;

// BMP decoder implementation structure
typedef struct {
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint8_t *rgb_buffer;   // RGB888 when opaque
    size_t rgb_buffer_size;
    uint8_t *rgba_buffer;  // RGBA8888 when an alpha mask is honored
    size_t rgba_buffer_size;
    bool has_alpha;
    bool initialized;
    uint32_t current_frame_delay_ms;
} bmp_decoder_data_t;

// Little-endian readers - the file buffer has no alignment guarantees.
static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t rd_s32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
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

// Validate a channel mask (nonzero, one contiguous run of ones, within the
// pixel word) and precompute its extraction parameters.
static bool bmp_chan_from_mask(uint32_t mask, uint32_t bitcount, bmp_chan_t *ch)
{
    if (mask == 0) {
        return false;
    }
    if (bitcount < 32 && (mask >> bitcount) != 0) {
        return false;
    }
    const uint32_t shift = (uint32_t)__builtin_ctz(mask);
    const uint32_t run = mask >> shift;
    if ((run & (run + 1u)) != 0) { // overflow to 0 for a full 32-bit mask is fine
        return false;
    }
    uint32_t bits = 0;
    for (uint32_t r = run; r != 0; r >>= 1) {
        bits++;
    }
    ch->shift = shift;
    ch->bits = bits;
    ch->max = run;
    return true;
}

// Extract one channel and scale it to 8 bits.
static inline uint8_t bmp_chan_extract(uint32_t pix, const bmp_chan_t *ch)
{
    const uint32_t v = (pix >> ch->shift) & ch->max;
    if (ch->bits == 8) {
        return (uint8_t)v;
    }
    if (ch->bits > 8) {
        return (uint8_t)(v >> (ch->bits - 8));
    }
    // Exact rounding scale for narrow channels (v*255/max), e.g. 5 -> 8 bits.
    return (uint8_t)((v * 255u + (ch->max >> 1)) / ch->max);
}

static inline void bmp_pal_put(const bmp_parse_t *bp, uint8_t *d, uint32_t idx)
{
    if (idx >= bp->pal_count) {
        idx = bp->pal_count - 1u; // forgiving clamp, matches lax real-world files
    }
    d[0] = bp->pal[idx][0];
    d[1] = bp->pal[idx][1];
    d[2] = bp->pal[idx][2];
}

// Parse and validate both headers, the palette, the channel masks, and the
// pixel-array bounds. On ESP_OK every field of @p bp is safe to decode from.
static esp_err_t bmp_parse_header(const uint8_t *data, size_t size, bmp_parse_t *bp)
{
    if (size < 14u + 12u) {
        ESP_LOGE(TAG, "File too small for BMP headers (%zu bytes)", size);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t off_bits = rd_u32(data + 10);
    const uint32_t hdr_size = rd_u32(data + 14);

    uint32_t width = 0, height = 0, bitcount = 0, clr_used = 0;
    uint32_t compression = BMP_BI_RGB;
    uint16_t planes = 0;
    size_t pal_entry_size = 4;

    if (hdr_size == 12) {
        // BITMAPCOREHEADER (OS/2 1.x): u16 dims, always bottom-up, BGR palette
        width = rd_u16(data + 18);
        height = rd_u16(data + 20);
        planes = rd_u16(data + 22);
        bitcount = rd_u16(data + 24);
        pal_entry_size = 3;
        if (bitcount != 1 && bitcount != 4 && bitcount != 8 && bitcount != 24) {
            ESP_LOGE(TAG, "Core header with unsupported bit depth %u", (unsigned)bitcount);
            return ESP_FAIL;
        }
    } else if (hdr_size == 40 || hdr_size == 52 || hdr_size == 56 ||
               hdr_size == 64 || hdr_size == 108 || hdr_size == 124) {
        if ((uint64_t)14u + hdr_size > size) {
            ESP_LOGE(TAG, "Truncated DIB header (%u declared)", (unsigned)hdr_size);
            return ESP_ERR_INVALID_SIZE;
        }
        const int32_t w_raw = rd_s32(data + 18);
        const int32_t h_raw = rd_s32(data + 22);
        planes = rd_u16(data + 26);
        bitcount = rd_u16(data + 28);
        compression = rd_u32(data + 30);
        clr_used = rd_u32(data + 46);
        if (w_raw <= 0 || h_raw == 0) {
            ESP_LOGE(TAG, "Invalid BMP dimensions: %ld x %ld", (long)w_raw, (long)h_raw);
            return ESP_ERR_INVALID_SIZE;
        }
        width = (uint32_t)w_raw;
        if (h_raw < 0) {
            bp->top_down = true;
            height = (uint32_t)(-(int64_t)h_raw);
        } else {
            height = (uint32_t)h_raw;
        }
    } else {
        ESP_LOGE(TAG, "Unsupported DIB header size %u", (unsigned)hdr_size);
        return ESP_FAIL;
    }

    if (planes != 1) {
        ESP_LOGE(TAG, "Unsupported plane count %u", (unsigned)planes);
        return ESP_FAIL;
    }
    if (width < 1 || width > BMP_MAX_DIM || height < 1 || height > BMP_MAX_DIM) {
        ESP_LOGE(TAG, "BMP dimensions out of range: %ux%u (cap %u)",
                 (unsigned)width, (unsigned)height, (unsigned)BMP_MAX_DIM);
        return ESP_ERR_INVALID_SIZE;
    }
    if (bitcount != 1 && bitcount != 4 && bitcount != 8 &&
        bitcount != 16 && bitcount != 24 && bitcount != 32) {
        ESP_LOGE(TAG, "Unsupported bit depth %u", (unsigned)bitcount);
        return ESP_FAIL;
    }

    switch (compression) {
    case BMP_BI_RGB:
        break;
    case BMP_BI_RLE8:
        if (bitcount != 8 || bp->top_down) {
            ESP_LOGE(TAG, "Invalid RLE8 file (bitcount=%u, top_down=%d)",
                     (unsigned)bitcount, (int)bp->top_down);
            return ESP_FAIL;
        }
        break;
    case BMP_BI_RLE4:
        if (bitcount != 4 || bp->top_down) {
            ESP_LOGE(TAG, "Invalid RLE4 file (bitcount=%u, top_down=%d)",
                     (unsigned)bitcount, (int)bp->top_down);
            return ESP_FAIL;
        }
        break;
    case BMP_BI_BITFIELDS:
    case BMP_BI_ALPHABITFIELDS:
        // OS/2 v2 (size 64) assigns 3/4 to Huffman-1D/RLE24 - different formats.
        // ALPHABITFIELDS is WinCE-only and its masks follow a 40-byte header.
        if ((bitcount != 16 && bitcount != 32) || hdr_size == 64 ||
            (compression == BMP_BI_ALPHABITFIELDS && hdr_size != 40)) {
            ESP_LOGE(TAG, "Unsupported BITFIELDS layout (bitcount=%u, header=%u)",
                     (unsigned)bitcount, (unsigned)hdr_size);
            return ESP_FAIL;
        }
        break;
    default:
        ESP_LOGE(TAG, "Unsupported compression %u", (unsigned)compression);
        return ESP_FAIL;
    }

    bp->width = width;
    bp->height = height;
    bp->bitcount = bitcount;
    bp->compression = compression;

    // Channel masks for the truecolor word formats.
    if (bitcount == 16 || bitcount == 32) {
        uint32_t r_mask, g_mask, b_mask, a_mask = 0;
        if (bitcount == 16) {
            r_mask = 0x7C00u; // BI_RGB 16-bit default is 5-5-5
            g_mask = 0x03E0u;
            b_mask = 0x001Fu;
        } else {
            r_mask = 0x00FF0000u; // BI_RGB 32-bit is fixed BGRX
            g_mask = 0x0000FF00u;
            b_mask = 0x000000FFu;
        }
        if (compression == BMP_BI_BITFIELDS || compression == BMP_BI_ALPHABITFIELDS) {
            if (hdr_size == 40) {
                // Masks follow the header (3 for BITFIELDS, 4 for ALPHABITFIELDS)
                const uint64_t mask_len = (compression == BMP_BI_ALPHABITFIELDS) ? 16u : 12u;
                if (14u + 40u + mask_len > size) {
                    ESP_LOGE(TAG, "Truncated BITFIELDS masks");
                    return ESP_ERR_INVALID_SIZE;
                }
                r_mask = rd_u32(data + 54);
                g_mask = rd_u32(data + 58);
                b_mask = rd_u32(data + 62);
                if (compression == BMP_BI_ALPHABITFIELDS) {
                    a_mask = rd_u32(data + 66);
                }
            } else {
                // header >= 52: masks live inside the header (already bounds-checked)
                r_mask = rd_u32(data + 14 + 40);
                g_mask = rd_u32(data + 14 + 44);
                b_mask = rd_u32(data + 14 + 48);
                if (hdr_size >= 56) {
                    a_mask = rd_u32(data + 14 + 52);
                }
            }
        }
        if (!bmp_chan_from_mask(r_mask, bitcount, &bp->r) ||
            !bmp_chan_from_mask(g_mask, bitcount, &bp->g) ||
            !bmp_chan_from_mask(b_mask, bitcount, &bp->b)) {
            ESP_LOGE(TAG, "Invalid color masks %08X/%08X/%08X",
                     (unsigned)r_mask, (unsigned)g_mask, (unsigned)b_mask);
            return ESP_FAIL;
        }
        if (a_mask != 0) {
            if (!bmp_chan_from_mask(a_mask, bitcount, &bp->a)) {
                ESP_LOGE(TAG, "Invalid alpha mask %08X", (unsigned)a_mask);
                return ESP_FAIL;
            }
            bp->has_alpha = true;
        }
    }

    // Palette (indexed formats only - BITFIELDS requires bitcount >= 16, so the
    // after-header mask words and the palette never coexist).
    uint64_t pixel_floor = 14u + hdr_size; // earliest legal bfOffBits
    if (compression == BMP_BI_BITFIELDS && hdr_size == 40) {
        pixel_floor += 12u;
    } else if (compression == BMP_BI_ALPHABITFIELDS) {
        pixel_floor += 16u;
    }
    if (bitcount <= 8) {
        uint32_t pal_count = (clr_used != 0) ? clr_used : (1u << bitcount);
        if (pal_count > 256u) {
            ESP_LOGE(TAG, "Palette count %u out of range", (unsigned)pal_count);
            return ESP_FAIL;
        }
        const uint64_t pal_off = 14u + hdr_size;
        const uint64_t pal_end = pal_off + (uint64_t)pal_count * pal_entry_size;
        if (pal_end > size || pal_end > off_bits) {
            ESP_LOGE(TAG, "Palette overruns file or pixel data");
            return ESP_ERR_INVALID_SIZE;
        }
        for (uint32_t i = 0; i < pal_count; i++) {
            const uint8_t *e = data + (size_t)pal_off + (size_t)i * pal_entry_size;
            bp->pal[i][0] = e[2]; // entries are stored BGR(X)
            bp->pal[i][1] = e[1];
            bp->pal[i][2] = e[0];
        }
        bp->pal_count = pal_count;
        pixel_floor = pal_end;
    }

    // Pixel array bounds.
    if (off_bits < pixel_floor || off_bits >= size) {
        ESP_LOGE(TAG, "Pixel data offset %u out of bounds", (unsigned)off_bits);
        return ESP_ERR_INVALID_SIZE;
    }
    bp->pixels = data + off_bits;
    bp->pixels_size = size - off_bits;

    if (compression != BMP_BI_RLE8 && compression != BMP_BI_RLE4) {
        const uint64_t stride = (((uint64_t)width * bitcount + 31u) / 32u) * 4u;
        if (stride * height > bp->pixels_size) {
            ESP_LOGE(TAG, "Truncated pixel data (%zu bytes for %ux%u @ %u bpp)",
                     bp->pixels_size, (unsigned)width, (unsigned)height, (unsigned)bitcount);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

// Decode any non-RLE variant into @p out (RGB888, or RGBA8888 when has_alpha).
// Bounds were fully validated by bmp_parse_header.
static void bmp_decode_uncompressed(const bmp_parse_t *bp, uint8_t *out)
{
    const uint32_t w = bp->width;
    const uint32_t h = bp->height;
    const size_t stride = (size_t)((((uint64_t)w * bp->bitcount + 31u) / 32u) * 4u);
    const size_t out_bpp = bp->has_alpha ? 4u : 3u;

    for (uint32_t fr = 0; fr < h; fr++) {
        const uint8_t *src = bp->pixels + (size_t)fr * stride;
        const uint32_t dr = bp->top_down ? fr : (h - 1u - fr);
        uint8_t *dst = out + (size_t)dr * w * out_bpp;

        switch (bp->bitcount) {
        case 1:
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t idx = (src[x >> 3] >> (7u - (x & 7u))) & 1u;
                bmp_pal_put(bp, dst + (size_t)x * 3u, idx);
            }
            break;
        case 4:
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t idx = (x & 1u) ? (src[x >> 1] & 0x0Fu) : (src[x >> 1] >> 4);
                bmp_pal_put(bp, dst + (size_t)x * 3u, idx);
            }
            break;
        case 8:
            for (uint32_t x = 0; x < w; x++) {
                bmp_pal_put(bp, dst + (size_t)x * 3u, src[x]);
            }
            break;
        case 16:
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t pix = rd_u16(src + (size_t)x * 2u);
                uint8_t *d = dst + (size_t)x * out_bpp;
                d[0] = bmp_chan_extract(pix, &bp->r);
                d[1] = bmp_chan_extract(pix, &bp->g);
                d[2] = bmp_chan_extract(pix, &bp->b);
                if (bp->has_alpha) {
                    d[3] = bmp_chan_extract(pix, &bp->a);
                }
            }
            break;
        case 24:
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *s = src + (size_t)x * 3u;
                uint8_t *d = dst + (size_t)x * 3u;
                d[0] = s[2]; // BGR -> RGB
                d[1] = s[1];
                d[2] = s[0];
            }
            break;
        case 32:
        default:
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t pix = rd_u32(src + (size_t)x * 4u);
                uint8_t *d = dst + (size_t)x * out_bpp;
                d[0] = bmp_chan_extract(pix, &bp->r);
                d[1] = bmp_chan_extract(pix, &bp->g);
                d[2] = bmp_chan_extract(pix, &bp->b);
                if (bp->has_alpha) {
                    d[3] = bmp_chan_extract(pix, &bp->a);
                }
            }
            break;
        }
    }
}

static inline void bmp_rle_put(const bmp_parse_t *bp, uint8_t *out,
                               uint32_t x, uint32_t fr, uint32_t idx)
{
    const uint32_t dr = bp->height - 1u - fr; // RLE is bottom-up only
    bmp_pal_put(bp, out + ((size_t)dr * bp->width + (size_t)x) * 3u, idx);
}

// Decode RLE8/RLE4 into @p out (RGB888, pre-zeroed by the caller so pixels
// skipped via EOL/EOB/delta - undefined per spec - render deterministic
// black). Pixel writes are clamped to the image rect; any *read* past EOF is
// a hard error (corrupt file). The cursor walks (x, file_row) bottom-up.
static esp_err_t bmp_decode_rle(const bmp_parse_t *bp, uint8_t *out)
{
    const bool rle4 = (bp->compression == BMP_BI_RLE4);
    const uint8_t *p = bp->pixels;
    const uint8_t *end = bp->pixels + bp->pixels_size;
    const uint32_t w = bp->width;
    const uint32_t h = bp->height;
    uint32_t x = 0;
    uint32_t fr = 0;

    while ((size_t)(end - p) >= 2u) {
        const uint8_t n = *p++;
        const uint8_t v = *p++;
        if (n > 0) {
            // Encoded run: n pixels of v (RLE4 alternates high/low nibble)
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t idx = rle4 ? ((i & 1u) ? (v & 0x0Fu) : (uint32_t)(v >> 4)) : v;
                if (x < w && fr < h) {
                    bmp_rle_put(bp, out, x, fr, idx);
                }
                x++;
            }
        } else if (v == 0) { // end of line
            x = 0;
            fr++;
        } else if (v == 1) { // end of bitmap
            return ESP_OK;
        } else if (v == 2) { // delta: move cursor right/up
            if ((size_t)(end - p) < 2u) {
                ESP_LOGE(TAG, "RLE delta runs past end of file");
                return ESP_ERR_INVALID_SIZE;
            }
            x += *p++;
            fr += *p++;
        } else {
            // Absolute run of v literal pixels, padded to a 16-bit boundary
            const uint32_t bytes = rle4 ? (((uint32_t)v + 1u) / 2u) : v;
            const uint32_t padded = (bytes + 1u) & ~1u;
            if ((size_t)(end - p) < padded) {
                ESP_LOGE(TAG, "RLE absolute run past end of file");
                return ESP_ERR_INVALID_SIZE;
            }
            for (uint32_t i = 0; i < v; i++) {
                const uint32_t idx = rle4 ? ((i & 1u) ? (p[i >> 1] & 0x0Fu) : (uint32_t)(p[i >> 1] >> 4))
                                          : p[i];
                if (x < w && fr < h) {
                    bmp_rle_put(bp, out, x, fr, idx);
                }
                x++;
            }
            p += padded;
        }
    }
    // No explicit end-of-bitmap marker. Tolerate a stream that ends cleanly
    // (some encoders omit the trailing 00 01); a dangling byte is corruption.
    if (p == end) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "RLE stream ends mid-opcode");
    return ESP_ERR_INVALID_SIZE;
}

esp_err_t bmp_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    if (!decoder || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size < 2 || data[0] != 'B' || data[1] != 'M') {
        ESP_LOGE(TAG, "Invalid BMP signature");
        return ESP_ERR_INVALID_ARG;
    }

    // Parse scratch is ~800 bytes (palette table); keep it off the task stack.
    bmp_parse_t *bp = (bmp_parse_t *)calloc(1, sizeof(bmp_parse_t));
    if (!bp) {
        ESP_LOGE(TAG, "Failed to allocate BMP parse state");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bmp_parse_header(data, size, bp);
    if (err != ESP_OK) {
        free(bp);
        return err;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)calloc(1, sizeof(bmp_decoder_data_t));
    if (!bmp_data) {
        ESP_LOGE(TAG, "Failed to allocate BMP decoder data");
        free(bp);
        return ESP_ERR_NO_MEM;
    }

    bmp_data->canvas_width = bp->width;
    bmp_data->canvas_height = bp->height;
    bmp_data->has_alpha = bp->has_alpha;
    bmp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    const size_t pixel_count = (size_t)bp->width * (size_t)bp->height;
    uint8_t *out;
    if (bp->has_alpha) {
        bmp_data->rgba_buffer_size = pixel_count * 4u;
        bmp_data->rgba_buffer = alloc_pixels(bmp_data->rgba_buffer_size);
        out = bmp_data->rgba_buffer;
    } else {
        bmp_data->rgb_buffer_size = pixel_count * 3u;
        bmp_data->rgb_buffer = alloc_pixels(bmp_data->rgb_buffer_size);
        out = bmp_data->rgb_buffer;
    }
    if (!out) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer (%zu bytes)",
                 bp->has_alpha ? bmp_data->rgba_buffer_size : bmp_data->rgb_buffer_size);
        free(bmp_data);
        free(bp);
        return ESP_ERR_NO_MEM;
    }

    const bool is_rle = (bp->compression == BMP_BI_RLE8 || bp->compression == BMP_BI_RLE4);
    if (is_rle) {
        memset(out, 0, bmp_data->rgb_buffer_size);
        err = bmp_decode_rle(bp, out);
    } else {
        bmp_decode_uncompressed(bp, out);
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        free(out);
        free(bmp_data);
        free(bp);
        return err;
    }

    bmp_data->initialized = true;

    animation_decoder_t *dec = (animation_decoder_t *)calloc(1, sizeof(animation_decoder_t));
    if (!dec) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        free(out);
        free(bmp_data);
        free(bp);
        return ESP_ERR_NO_MEM;
    }

    dec->type = ANIMATION_DECODER_TYPE_BMP;
    dec->impl.bmp.bmp_decoder = bmp_data;
    *decoder = dec;

    ESP_LOGI(TAG, "BMP decoder initialized: %ux%u, %u-bit%s%s%s",
             (unsigned)bp->width, (unsigned)bp->height, (unsigned)bp->bitcount,
             is_rle ? ", RLE" : "",
             bp->top_down ? ", top-down" : "",
             bp->has_alpha ? ", alpha" : "");
    free(bp);
    return ESP_OK;
}

esp_err_t bmp_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info)
{
    if (!decoder || !info || decoder->type != ANIMATION_DECODER_TYPE_BMP) {
        return ESP_ERR_INVALID_ARG;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)decoder->impl.bmp.bmp_decoder;
    if (!bmp_data || !bmp_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    info->canvas_width = bmp_data->canvas_width;
    info->canvas_height = bmp_data->canvas_height;
    info->frame_count = 1; // BMP is always single frame
    info->has_transparency = bmp_data->has_alpha;
    info->pixel_format = ANIMATION_PIXEL_FORMAT_RGB888;
    // The entire file is decoded synchronously inside bmp_decoder_init;
    // nothing references the source bytes afterwards.
    info->source_consumed = true;

    return ESP_OK;
}

esp_err_t bmp_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    if (!decoder || !rgba_buffer || decoder->type != ANIMATION_DECODER_TYPE_BMP) {
        return ESP_ERR_INVALID_ARG;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)decoder->impl.bmp.bmp_decoder;
    if (!bmp_data || !bmp_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (bmp_data->has_alpha) {
        if (!bmp_data->rgba_buffer || bmp_data->rgba_buffer_size == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(rgba_buffer, bmp_data->rgba_buffer, bmp_data->rgba_buffer_size);
    } else {
        if (!bmp_data->rgb_buffer || bmp_data->rgb_buffer_size == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t pixel_count = (size_t)bmp_data->canvas_width * (size_t)bmp_data->canvas_height;
        const uint8_t *src = bmp_data->rgb_buffer;
        for (size_t i = 0; i < pixel_count; i++) {
            rgba_buffer[i * 4 + 0] = src[i * 3 + 0];
            rgba_buffer[i * 4 + 1] = src[i * 3 + 1];
            rgba_buffer[i * 4 + 2] = src[i * 3 + 2];
            rgba_buffer[i * 4 + 3] = 255;
        }
        // Opaque branch: drop the intermediate. See decode_next_rgb for rationale.
        free(bmp_data->rgb_buffer);
        bmp_data->rgb_buffer = NULL;
        bmp_data->rgb_buffer_size = 0;
    }
    bmp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t bmp_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer)
{
    if (!decoder || !rgb_buffer || decoder->type != ANIMATION_DECODER_TYPE_BMP) {
        return ESP_ERR_INVALID_ARG;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)decoder->impl.bmp.bmp_decoder;
    if (!bmp_data || !bmp_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!bmp_data->has_alpha) {
        if (!bmp_data->rgb_buffer || bmp_data->rgb_buffer_size == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(rgb_buffer, bmp_data->rgb_buffer, bmp_data->rgb_buffer_size);
        bmp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
        // Opaque BMP never re-decodes (no bg-recompose path), so the
        // intermediate buffer is dead weight after the caller's b1 holds
        // the pixels. Free it to halve the per-asset PSRAM footprint.
        // For alpha BMPs we keep rgba_buffer alive because the renderer's
        // static fast path re-invokes decode_next_rgb on background-color
        // change to recomposite against the new bg.
        free(bmp_data->rgb_buffer);
        bmp_data->rgb_buffer = NULL;
        bmp_data->rgb_buffer_size = 0;
        return ESP_OK;
    }

    if (!bmp_data->rgba_buffer || bmp_data->rgba_buffer_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    flatten_rgba_to_rgb_bg(bmp_data->rgba_buffer, rgb_buffer,
                           (size_t)bmp_data->canvas_width * (size_t)bmp_data->canvas_height);

    bmp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;
    return ESP_OK;
}

esp_err_t bmp_decoder_reset(animation_decoder_t *decoder)
{
    if (!decoder || decoder->type != ANIMATION_DECODER_TYPE_BMP) {
        return ESP_ERR_INVALID_ARG;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)decoder->impl.bmp.bmp_decoder;
    if (!bmp_data || !bmp_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // BMP is static, so reset just restores the delay
    bmp_data->current_frame_delay_ms = STATIC_IMAGE_FRAME_DELAY_MS;

    return ESP_OK;
}

esp_err_t bmp_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms)
{
    if (!decoder || !delay_ms || decoder->type != ANIMATION_DECODER_TYPE_BMP) {
        return ESP_ERR_INVALID_ARG;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)decoder->impl.bmp.bmp_decoder;
    if (!bmp_data || !bmp_data->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *delay_ms = bmp_data->current_frame_delay_ms;
    return ESP_OK;
}

void bmp_decoder_unload(animation_decoder_t **decoder)
{
    if (!decoder || !*decoder) {
        return;
    }

    animation_decoder_t *dec = *decoder;
    if (dec->type != ANIMATION_DECODER_TYPE_BMP) {
        return;
    }

    bmp_decoder_data_t *bmp_data = (bmp_decoder_data_t *)dec->impl.bmp.bmp_decoder;
    if (bmp_data) {
        if (bmp_data->rgb_buffer) {
            free(bmp_data->rgb_buffer);
            bmp_data->rgb_buffer = NULL;
        }
        if (bmp_data->rgba_buffer) {
            free(bmp_data->rgba_buffer);
            bmp_data->rgba_buffer = NULL;
        }
        free(bmp_data);
    }

    free(dec);
    *decoder = NULL;
}
