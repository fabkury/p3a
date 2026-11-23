// SPDX-License-Identifier: Apache-2.0
// The animation player. See animation_player.h.
//
// Two clips: the front one is on the panel, the back one is where the next
// is prepared. Who may touch which:
//
//   front clip   the display pipeline's producer task, only.
//   back clip    the player's task, except while it is "staged" (loaded and
//                waiting): then the producer may adopt it, which makes it the
//                front clip and the old front clip the back one, to be
//                unloaded by the player's task.
//
// The lock guards the few words that say which is which. It is never held
// across a load, a decode or a draw, so the producer waits on it for
// microseconds at most.

#include <stdatomic.h>
#include <string.h>

#include "animation_player.h"
#include "display_renderer.h"
#include "esp_log.h"
#include "frame_trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_err.h"
#include "p3a_task.h"
#include "player_clip.h"

static const char *TAG = "player";

static player_clip_t s_clips[2];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

// Guarded by s_lock.
static int s_front;               // index of the clip on the panel
static uint32_t s_front_token;
static bool s_staged;             // the back clip is loaded and waits to be adopted
static uint32_t s_staged_token;
static bool s_back_retired;       // the back clip holds the previous artwork, to be unloaded
static bool s_pending;            // a request waits for the player's task
static bool s_pending_stop;
static animation_player_request_t s_request;
static uint32_t s_shown_token;    // adopted by the producer, not yet reported
static bool s_busy;               // the player's task is loading

// A copy of the front clip's counters, for status readers.
static _Atomic uint32_t s_frames_shown, s_draws;
static atomic_bool s_decode_failed;

static animation_player_listener_t s_listener;
static void *s_listener_ctx;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// --- the display pipeline's side ------------------------------------------------

static int artwork_source(uint8_t *buffer, bool must_redraw, void *ctx)
{
    (void)ctx;
    bool adopted = false;
    LOCK();
    if (s_staged) {
        s_front ^= 1;
        s_front_token = s_staged_token;
        s_shown_token = s_staged_token;
        s_staged = false;
        s_back_retired = true;
        adopted = true;
    }
    player_clip_t *clip = &s_clips[s_front];
    UNLOCK();

    if (adopted) {
        // Frames already queued belong to the previous artwork: drop them, so
        // that the new one appears at the next refresh and not after them.
        display_renderer_note_content_discontinuity();
        must_redraw = true;
    }
    const int delay_ms = player_clip_render(clip, buffer, must_redraw);
    atomic_store(&s_frames_shown, clip->frames_shown);
    atomic_store(&s_draws, clip->draws);
    atomic_store(&s_decode_failed, clip->decode_failed);
    if (adopted) {
        xTaskNotifyGive(s_task);  // report "shown" and free the previous artwork
    }
    return delay_ms;
}

// --- the player's task ----------------------------------------------------------

static void report_failed(uint32_t token, esp_err_t err)
{
    if (token != 0 && s_listener.failed) {
        s_listener.failed(token, err, s_listener_ctx);
    }
}

// Reports an adoption and frees what it replaced.
static void settle_adoption(void)
{
    LOCK();
    const uint32_t shown = s_shown_token;
    s_shown_token = 0;
    // The back clip is ours to free unless a new one is staged in it, which
    // cannot be: only this task stages, and it settles first.
    const bool retire = s_back_retired && !s_staged;
    s_back_retired = false;
    player_clip_t *back = &s_clips[s_front ^ 1];
    UNLOCK();
    if (retire) {
        player_clip_unload(back);
    }
    if (shown != 0 && s_listener.shown) {
        s_listener.shown(shown, s_listener_ctx);
    }
}

static void serve_request(void)
{
    animation_player_request_t request;
    bool stop;
    uint32_t replaced = 0;
    LOCK();
    if (!s_pending) {
        UNLOCK();
        return;
    }
    request = s_request;
    stop = s_pending_stop;
    s_pending = false;
    if (s_staged) {
        // Loaded but never shown, and now replaced. Taken back before the
        // producer can adopt it.
        s_staged = false;
        replaced = s_staged_token;
        s_back_retired = true;
    }
    s_busy = true;
    UNLOCK();

    settle_adoption();  // frees the back clip, whichever way it came to hold something
    report_failed(replaced, ESP_ERR_NOT_FINISHED);

    player_clip_t *back = &s_clips[s_front ^ 1];  // s_front changes only on adoption, and nothing is staged
    esp_err_t err = ESP_OK;
    if (!stop) {
        frame_trace_mark(FT_MARK_LOADER_LOAD, FT_PHASE_BEGIN, 0);
        const int64_t t0 = frame_trace_now_us();
        err = player_clip_load(back, request.path, request.smooth);
        frame_trace_mark(FT_MARK_LOADER_LOAD, FT_PHASE_END, err == ESP_OK ? 0 : 1);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "loaded %s: %ux%u, %u frames, %d ms", request.path, (unsigned)back->info.canvas_width,
                     (unsigned)back->info.canvas_height, (unsigned)back->info.frame_count,
                     (int)((frame_trace_now_us() - t0) / 1000));
        } else {
            ESP_LOGW(TAG, "cannot play %s: %s", request.path, p3a_err_to_name(err));
        }
    }

    // The listener hears "ready" before the artwork is staged, so that its
    // "no" comes before the display pipeline could adopt it.
    bool wanted = true;
    if (err == ESP_OK && s_listener.ready) {
        const animation_player_loaded_t loaded = {  // all zero for "play nothing"
            .width = stop ? 0 : (uint32_t)back->info.canvas_width,
            .height = stop ? 0 : (uint32_t)back->info.canvas_height,
            .frame_count = stop ? 0 : (uint32_t)back->info.frame_count,
        };
        wanted = s_listener.ready(request.token, &loaded, s_listener_ctx);
    }

    LOCK();
    s_busy = false;
    const bool superseded = s_pending;
    if (err == ESP_OK && wanted && !superseded) {
        s_staged = true;
        s_staged_token = request.token;
    }
    UNLOCK();

    if (err != ESP_OK) {
        report_failed(request.token, err);
    } else if (!wanted) {
        player_clip_unload(back);
    } else if (superseded) {
        player_clip_unload(back);
        report_failed(request.token, ESP_ERR_NOT_FINISHED);
    }
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        settle_adoption();
        bool more = true;
        while (more) {
            serve_request();
            LOCK();
            more = s_pending;
            UNLOCK();
        }
    }
}

// --- the callers' side ----------------------------------------------------------

static esp_err_t submit(const animation_player_request_t *request, bool stop)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (request->token == 0 || (!stop && request->path[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t replaced = 0;
    LOCK();
    if (s_pending) {
        replaced = s_request.token;  // never started
    }
    s_request = *request;
    s_pending_stop = stop;
    s_pending = true;
    UNLOCK();
    // On the caller's task, unlike every other report: the request never
    // reached the player's task, and a caller that asks faster than files
    // load could discard any number of them.
    report_failed(replaced, ESP_ERR_NOT_FINISHED);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t animation_player_play(const animation_player_request_t *request)
{
    if (request == NULL || memchr(request->path, '\0', sizeof(request->path)) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return submit(request, false);
}

esp_err_t animation_player_stop(uint32_t token)
{
    animation_player_request_t request = {.token = token};
    return submit(&request, true);
}

void animation_player_set_listener(const animation_player_listener_t *listener, void *ctx)
{
    s_listener = listener ? *listener : (animation_player_listener_t){0};
    s_listener_ctx = ctx;
}

void animation_player_get_status(animation_player_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    const player_clip_t *front = &s_clips[s_front];
    out->token = s_front_token;
    // The size and frame count are set at load and not changed while the
    // clip is the front one; reading them here is safe.
    out->width = front->info.canvas_width;
    out->height = front->info.canvas_height;
    out->frame_count = front->loaded ? (uint32_t)front->info.frame_count : 0;
    out->loading = s_pending || s_busy || s_staged;
    UNLOCK();
    out->frames_shown = atomic_load(&s_frames_shown);
    out->draws = atomic_load(&s_draws);
    out->decode_failed = atomic_load(&s_decode_failed);
    out->loops = out->frame_count > 1 ? out->frames_shown / out->frame_count : 0;
}

esp_err_t animation_player_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // 8 KB: the decoders' deepest call chain measured 3.1 KB (M2.1 part 1).
    p3a_task_config_t config = P3A_TASK_CONFIG("player", player_task, NULL, 8192, P3A_TASK_APP);
    esp_err_t err = p3a_task_create(&config, &s_task);
    if (err != ESP_OK) {
        return err;
    }
    display_renderer_set_source(DISPLAY_SOURCE_ARTWORK, artwork_source, NULL);
    return ESP_OK;
}
