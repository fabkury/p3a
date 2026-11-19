// SPDX-License-Identifier: Apache-2.0
// A clip: one file opened for playing. See player_clip.h.

#include <stdlib.h>
#include <string.h>

#include "display_ppa_upscaler.h"
#include "display_renderer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "frame_trace.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "player_clip.h"
#include "player_file.h"
#include "psram_alloc.h"

static const char *TAG = "player";

// A picture whose decoder has failed is redrawn at this pace when something
// asks for a redraw; nothing about it changes by itself.
#define FROZEN_FRAME_MS 500

#define LOOKUP_LEN ((P3A_DISPLAY_WIDTH > P3A_DISPLAY_HEIGHT) ? P3A_DISPLAY_WIDTH : P3A_DISPLAY_HEIGHT)

static animation_decoder_type_t decoder_type(player_format_t format)
{
    switch (format) {
        case PLAYER_FORMAT_WEBP: return ANIMATION_DECODER_TYPE_WEBP;
        case PLAYER_FORMAT_GIF:  return ANIMATION_DECODER_TYPE_GIF;
        case PLAYER_FORMAT_PNG:  return ANIMATION_DECODER_TYPE_PNG;
        case PLAYER_FORMAT_JPEG: return ANIMATION_DECODER_TYPE_JPEG;
        default:                 return ANIMATION_DECODER_TYPE_BMP;
    }
}

// What a decoder's refusal means to whoever asked for the file. A decoder
// says so itself when memory ran out or the picture is over the size limit
// (the file may be sound, and must not be treated as damaged and deleted
// for it); anything else is damage.
static esp_err_t classify_decoder_error(esp_err_t err)
{
    if (err == ESP_ERR_NO_MEM || err == P3A_ERR_NOT_PLAYABLE) {
        return err;
    }
    return P3A_ERR_CORRUPT;
}

static void build_fit(player_clip_t *clip, int rotation)
{
    const bool quarter = rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270;
    const int w = (int)clip->info.canvas_width, h = (int)clip->info.canvas_height;
    player_fit_compute(w, h, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, quarter, &clip->fit);
    player_fit_fill_lookup(clip->lookup_x, LOOKUP_LEN, clip->fit.lookup_x_len, w);
    player_fit_fill_lookup(clip->lookup_y, LOOKUP_LEN, clip->fit.lookup_y_len, h);
    clip->fit_rotation = rotation;
}

void player_clip_unload(player_clip_t *clip)
{
    animation_decoder_unload(&clip->decoder);
    free(clip->file_data);
    free(clip->frame[0]);
    free(clip->frame[1]);
    heap_caps_free(clip->lookup_x);
    heap_caps_free(clip->lookup_y);
    memset(clip, 0, sizeof(*clip));
}

esp_err_t player_clip_load(player_clip_t *clip, const char *path, bool smooth)
{
    memset(clip, 0, sizeof(*clip));
    clip->smooth = smooth;

    esp_err_t err = player_file_read(path, &clip->file_data, &clip->file_size);
    if (err != ESP_OK) {
        return err;
    }
    const player_format_t format = player_format_sniff(clip->file_data, clip->file_size);
    if (format == PLAYER_FORMAT_UNKNOWN) {
        player_clip_unload(clip);
        return P3A_ERR_NOT_PLAYABLE;
    }
    err = animation_decoder_init(&clip->decoder, decoder_type(format), clip->file_data, clip->file_size);
    if (err == ESP_OK) {
        err = animation_decoder_get_info(clip->decoder, &clip->info);
    }
    if (err != ESP_OK) {
        player_clip_unload(clip);
        return classify_decoder_error(err);
    }
    const bool still = clip->info.frame_count <= 1;
    if (still && clip->info.source_consumed) {
        // The decoder holds the whole picture; the file's bytes are dead
        // weight for as long as it is shown (0.1 to 2 MiB).
        free(clip->file_data);
        clip->file_data = NULL;
        clip->file_size = 0;
    }

    const size_t frame_bytes = (size_t)clip->info.canvas_width * clip->info.canvas_height * 3;
    clip->frame[0] = psram_malloc(frame_bytes);
    // A still is drawn from its one frame for as long as it is up; a second
    // buffer would only be held, and for a large still it is megabytes.
    clip->frame[1] = still ? NULL : psram_malloc(frame_bytes);
    // Internal RAM: the upscaler reads these tables for every pixel of every
    // frame, on the render path. 2.9 KB a clip.
    clip->lookup_x = heap_caps_malloc(LOOKUP_LEN * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    clip->lookup_y = heap_caps_malloc(LOOKUP_LEN * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (clip->frame[0] == NULL || (!still && clip->frame[1] == NULL) || clip->lookup_x == NULL ||
        clip->lookup_y == NULL) {
        player_clip_unload(clip);
        return ESP_ERR_NO_MEM;
    }
    build_fit(clip, display_renderer_get_rotation());

    // The first frame is decoded here, on the player's task, so that the
    // display pipeline only has to scale it when the clip is adopted.
    err = animation_decoder_decode_next_rgb(clip->decoder, clip->frame[0]);
    if (err != ESP_OK) {
        player_clip_unload(clip);
        return classify_decoder_error(err);
    }
    uint32_t delay = ANIMATION_DECODER_STILL_FRAME_DELAY_MS;
    (void)animation_decoder_get_frame_delay(clip->decoder, &delay);
    clip->first_delay_ms = delay < 1 ? 1 : delay;
    clip->first_pending = true;
    clip->last_good = clip->frame[0];
    clip->next_frame = 1;
    clip->loaded = true;
    return ESP_OK;
}

static void scale_to_panel(player_clip_t *clip, const uint8_t *src, uint8_t *dest)
{
    clip->draws++;
    const display_rotation_t rotation = (display_rotation_t)clip->fit_rotation;
    const int w = (int)clip->info.canvas_width, h = (int)clip->info.canvas_height;
    if (clip->smooth &&
        display_ppa_upscale_rgb(src, w, h, dest, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, rotation) == ESP_OK) {
        return;
    }
    display_renderer_parallel_upscale_rgb(src, w, h, dest, clip->lookup_x, clip->lookup_y, clip->fit.offset_x,
                                          clip->fit.offset_y, clip->fit.scaled_w, clip->fit.scaled_h,
                                          clip->fit.has_borders, rotation);
}

int player_clip_render(player_clip_t *clip, uint8_t *dest, bool must_redraw)
{
    if (!clip->loaded) {
        if (!must_redraw) {
            return DISPLAY_FRAME_UNCHANGED;
        }
        memset(dest, 0, display_renderer_buffer_bytes());
        return ANIMATION_DECODER_STILL_FRAME_DELAY_MS;
    }
    const int rotation = display_renderer_get_rotation();
    if (rotation != clip->fit_rotation) {
        build_fit(clip, rotation);
        must_redraw = true;
    }
    // The background colour is in every frame twice: under transparent
    // pixels (the decoder puts it there) and in the borders (the scaler).
    const uint32_t background = animation_decoder_background_generation();
    const bool background_changed = background != clip->background_seen;
    clip->background_seen = background;

    if (clip->first_pending) {
        clip->first_pending = false;
        scale_to_panel(clip, clip->frame[0], dest);
        clip->frames_shown++;
        return (int)clip->first_delay_ms;
    }

    const bool still = clip->info.frame_count <= 1;
    if (still && !clip->decode_failed) {
        if (background_changed && clip->info.has_transparency) {
            // Decoded again so that the decoder composes it over the new colour.
            if (animation_decoder_decode_next_rgb(clip->decoder, clip->frame[0]) != ESP_OK) {
                ESP_LOGW(TAG, "a still could not be composed over the new background");
                clip->decode_failed = true;
            }
        }
        if (!must_redraw && !background_changed) {
            return DISPLAY_FRAME_UNCHANGED;
        }
        scale_to_panel(clip, clip->frame[0], dest);
        return ANIMATION_DECODER_STILL_FRAME_DELAY_MS;
    }

    if (!clip->decode_failed) {
        uint8_t *into = clip->frame[clip->next_frame];
        const int64_t decode_t0 = frame_trace_now_us();
        esp_err_t err = animation_decoder_decode_next_rgb(clip->decoder, into);
        if (err == ESP_ERR_INVALID_STATE) {
            // The end of the animation. Every format starts over, whatever
            // loop count the file declares: some decoders do it themselves
            // (GIF), the others say so and are reset here.
            animation_decoder_reset(clip->decoder);
            err = animation_decoder_decode_next_rgb(clip->decoder, into);
        }
        const int64_t decode_t1 = frame_trace_now_us();
        (void)decode_t0;  // read only when the frame trace is compiled in
        (void)decode_t1;
        if (err == ESP_OK) {
            uint32_t delay = 1;
            (void)animation_decoder_get_frame_delay(clip->decoder, &delay);
            clip->last_good = into;
            clip->next_frame ^= 1;
            scale_to_panel(clip, into, dest);
            frame_trace_producer_split((uint32_t)(decode_t1 - decode_t0),
                                       (uint32_t)(frame_trace_now_us() - decode_t1));
            clip->frames_shown++;
            return delay < 1 ? 1 : (int)delay;
        }
        // Latched: a damaged file is not asked again for every frame. The
        // picture stays on its last good frame until the next artwork.
        ESP_LOGW(TAG, "decoding stopped after %u frames: %s", (unsigned)clip->frames_shown, p3a_err_to_name(err));
        clip->decode_failed = true;
        must_redraw = true;
    }
    if (!must_redraw && !background_changed) {
        return DISPLAY_FRAME_UNCHANGED;
    }
    scale_to_panel(clip, clip->last_good, dest);
    return FROZEN_FRAME_MS;
}
