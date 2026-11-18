// SPDX-License-Identifier: Apache-2.0
// Test hook for the decoders: send a file, get back what it decodes to.
//
//   POST /api/v2/test/decode?type=<webp|gif|png|jpeg|bmp>[&frames=N]
//        body: the file. Answers a description: size, frame count, and for
//        each of the first N frames (default and at most 64) its CRC-32, its
//        delay and how long it took to decode; then what happens after a
//        reset and at the end of the animation.
//   POST /api/v2/test/decode?type=...&frame=K
//        body: the file. Answers frame K itself, as raw RGB888, with the
//        size in the X-P3A-Width and X-P3A-Height headers.
//
// The decoding runs on a task of its own with a large stack, one request at
// a time, and the answer says how much of that stack was used: the player
// needs to know how much to give its own decode task.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animation_decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_crc32.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_task.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"

#define MAX_FILE_BYTES  (4 * 1024 * 1024)
#define MAX_FRAMES      64
#define WORKER_STACK    (32 * 1024)

typedef struct {
    uint32_t crc;
    uint32_t delay_ms;
    uint32_t decode_us;
} frame_note_t;

typedef struct {
    // in
    animation_decoder_type_t type;
    const uint8_t *file;
    size_t file_len;
    int max_frames;
    int want_frame;  // -1: none
    // out
    esp_err_t init_err;
    animation_decoder_info_t info;
    int decoded;
    esp_err_t stop_err;       // why decoding stopped before max_frames, or ESP_OK
    frame_note_t frames[MAX_FRAMES];
    bool reset_ok;            // after a reset the first frame decodes to the same pixels
    uint8_t *raw;             // the wanted frame, to be freed by the handler
    size_t raw_len;
    uint32_t init_us;
    uint32_t stack_used;
} decode_job_t;

static SemaphoreHandle_t s_request;  // given by the handler
static SemaphoreHandle_t s_done;     // given by the worker
static SemaphoreHandle_t s_one_at_a_time;
static decode_job_t *s_job;
static TaskHandle_t s_worker;

static void run(decode_job_t *job)
{
    animation_decoder_t *decoder = NULL;
    int64_t t0 = esp_timer_get_time();
    job->init_err = animation_decoder_init(&decoder, job->type, job->file, job->file_len);
    job->init_us = (uint32_t)(esp_timer_get_time() - t0);
    if (job->init_err != ESP_OK) {
        return;
    }
    job->init_err = animation_decoder_get_info(decoder, &job->info);
    const size_t frame_bytes = (size_t)job->info.canvas_width * job->info.canvas_height * 3;
    uint8_t *pixels = job->init_err == ESP_OK ? psram_malloc(frame_bytes) : NULL;
    if (job->init_err == ESP_OK && pixels == NULL) {
        job->init_err = ESP_ERR_NO_MEM;
    }
    if (job->init_err != ESP_OK) {
        animation_decoder_unload(&decoder);
        return;
    }

    int limit = job->want_frame >= 0 ? job->want_frame + 1 : job->max_frames;
    for (int i = 0; i < limit; i++) {
        t0 = esp_timer_get_time();
        esp_err_t err = animation_decoder_decode_next_rgb(decoder, pixels);
        uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
        if (err != ESP_OK) {
            job->stop_err = err;
            break;
        }
        if (i < MAX_FRAMES) {
            job->frames[i].crc = p3a_crc32(0, pixels, frame_bytes);
            job->frames[i].decode_us = took;
            animation_decoder_get_frame_delay(decoder, &job->frames[i].delay_ms);
        }
        job->decoded = i + 1;
        if (i == job->want_frame) {
            job->raw = psram_malloc(frame_bytes);
            if (job->raw != NULL) {
                memcpy(job->raw, pixels, frame_bytes);
                job->raw_len = frame_bytes;
            }
        }
        // A still image has one frame however often it is asked for.
        if (job->info.frame_count <= 1 && job->want_frame < 0) {
            break;
        }
    }
    if (job->want_frame < 0 && job->decoded > 0) {
        job->reset_ok = animation_decoder_reset(decoder) == ESP_OK &&
                        animation_decoder_decode_next_rgb(decoder, pixels) == ESP_OK &&
                        p3a_crc32(0, pixels, frame_bytes) == job->frames[0].crc;
    }
    free(pixels);
    animation_decoder_unload(&decoder);
}

static void worker(void *arg)
{
    (void)arg;
    while (true) {
        xSemaphoreTake(s_request, portMAX_DELAY);
        run(s_job);
        s_job->stack_used = WORKER_STACK - (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        xSemaphoreGive(s_done);
    }
}

static bool type_of(const char *name, animation_decoder_type_t *out)
{
    static const char *const names[] = {"webp", "gif", "png", "jpeg", "bmp"};
    for (int i = 0; i < ANIMATION_DECODER_TYPE_COUNT_; i++) {
        if (strcmp(name, names[i]) == 0) {
            *out = (animation_decoder_type_t)i;
            return true;
        }
    }
    return false;
}

static void post_decode(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    char text[12];
    static decode_job_t job;  // large; guarded by s_one_at_a_time
    animation_decoder_type_t type;
    if (!p3a_http_query(req, "type", text, sizeof(text)) || !type_of(text, &type)) {
        p3a_http_fail(req, 400, "INVALID", "type must be one of webp, gif, png, jpeg, bmp");
        return;
    }
    size_t len = raw->content_len;
    if (len == 0 || len > MAX_FILE_BYTES) {
        p3a_http_fail(req, len == 0 ? 400 : 413, len == 0 ? "INVALID" : "TOO_LARGE", "send the file as the body");
        return;
    }
    uint8_t *file = psram_malloc(len);
    if (file == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "holding the file");
        return;
    }
    for (size_t got = 0; got < len;) {
        int n = httpd_req_recv(raw, (char *)file + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(file);
            p3a_http_fail(req, 400, "READ_BODY", "the file did not arrive");
            return;
        }
        got += (size_t)n;
    }

    xSemaphoreTake(s_one_at_a_time, portMAX_DELAY);
    memset(&job, 0, sizeof(job));
    job.type = type;
    job.file = file;
    job.file_len = len;
    job.max_frames = p3a_http_query(req, "frames", text, sizeof(text)) ? atoi(text) : MAX_FRAMES;
    if (job.max_frames < 1 || job.max_frames > MAX_FRAMES) {
        job.max_frames = MAX_FRAMES;
    }
    job.want_frame = p3a_http_query(req, "frame", text, sizeof(text)) ? atoi(text) : -1;
    s_job = &job;
    xSemaphoreGive(s_request);
    xSemaphoreTake(s_done, portMAX_DELAY);
    free(file);

    if (job.init_err != ESP_OK) {
        p3a_http_fail(req, 422, "NOT_DECODED", "%s", p3a_err_to_name(job.init_err));
    } else if (job.want_frame >= 0) {
        if (job.raw == NULL) {
            p3a_http_fail(req, 422, "NO_SUCH_FRAME", "decoding stopped after %d frame(s): %s", job.decoded,
                          p3a_err_to_name(job.stop_err));
        } else {
            char w[12], h[12];
            snprintf(w, sizeof(w), "%u", (unsigned)job.info.canvas_width);
            snprintf(h, sizeof(h), "%u", (unsigned)job.info.canvas_height);
            httpd_resp_set_type(raw, "application/octet-stream");
            httpd_resp_set_hdr(raw, "X-P3A-Width", w);
            httpd_resp_set_hdr(raw, "X-P3A-Height", h);
            httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
            httpd_resp_send(raw, (const char *)job.raw, (ssize_t)job.raw_len);
            p3a_http_mark_answered(req);
        }
    } else {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "width", job.info.canvas_width);
        cJSON_AddNumberToObject(data, "height", job.info.canvas_height);
        cJSON_AddNumberToObject(data, "frame_count", (double)job.info.frame_count);
        cJSON_AddBoolToObject(data, "has_transparency", job.info.has_transparency);
        cJSON_AddBoolToObject(data, "source_consumed", job.info.source_consumed);
        cJSON_AddNumberToObject(data, "init_us", job.init_us);
        cJSON_AddNumberToObject(data, "decoded", job.decoded);
        cJSON_AddStringToObject(data, "stopped", job.stop_err == ESP_OK ? "" : p3a_err_to_name(job.stop_err));
        cJSON_AddBoolToObject(data, "reset_ok", job.reset_ok);
        cJSON_AddNumberToObject(data, "stack_used", job.stack_used);
        cJSON_AddNumberToObject(data, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON *frames = cJSON_AddArrayToObject(data, "frames");
        for (int i = 0; i < job.decoded && i < MAX_FRAMES; i++) {
            cJSON *frame = cJSON_CreateObject();
            char crc[12];
            snprintf(crc, sizeof(crc), "%08x", (unsigned)job.frames[i].crc);
            cJSON_AddStringToObject(frame, "crc", crc);
            cJSON_AddNumberToObject(frame, "delay_ms", job.frames[i].delay_ms);
            cJSON_AddNumberToObject(frame, "decode_us", job.frames[i].decode_us);
            cJSON_AddItemToArray(frames, frame);
        }
        p3a_http_ok(req, data);
    }
    free(job.raw);
    xSemaphoreGive(s_one_at_a_time);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_POST, P3A_HTTP_RAW, "/api/v2/test/decode", post_decode},
};

void p3a_test_decode_start(void)
{
    s_request = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    s_one_at_a_time = xSemaphoreCreateMutex();
    p3a_task_config_t task = P3A_TASK_CONFIG("p3a_test_dec", worker, NULL, WORKER_STACK, P3A_TASK_APP);
    if (s_request == NULL || s_done == NULL || s_one_at_a_time == NULL || p3a_task_create(&task, &s_worker) != ESP_OK) {
        ESP_LOGE("p3a_test", "the decode hook did not start");
        return;
    }
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
