// SPDX-License-Identifier: Apache-2.0
// Test console commands for the util layer: tasks, errors, jobs, events,
// memory pressure.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_job.h"
#include "p3a_mem.h"
#include "p3a_test_internal.h"

// --- tasks and errors -----------------------------------------------------------

// Reports how the calling task (the console itself, class BACKGROUND) was
// created, so a device test can check that the task helper applies its policy.
static void cmd_task(const char *args)
{
    (void)args;
    char json[160];
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"priority\":%u,\"core\":%d,\"psram_stack\":%s}",
             pcTaskGetName(NULL), (unsigned)uxTaskPriorityGet(NULL), (int)xPortGetCoreID(),
             esp_ptr_external_ram(pxTaskGetStackStart(NULL)) ? "true" : "false");
    p3a_test_reply_ok(json);
}

// !errname <code>
static void cmd_errname(const char *args)
{
    char *end = NULL;
    long code = strtol(args, &end, 0);
    if (end == args) {
        p3a_test_reply_err("usage: !errname <code>");
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"p3a\":%s}", p3a_err_to_name((esp_err_t)code),
             p3a_err_is_p3a((esp_err_t)code) ? "true" : "false");
    p3a_test_reply_ok(json);
}

// --- jobs -----------------------------------------------------------------------

// Succeeds after a short while, reporting progress on the way.
static esp_err_t job_ok(void *arg)
{
    (void)arg;
    for (int percent = 10; percent <= 90; percent += 20) {
        p3a_job_set_progress((uint8_t)percent);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return ESP_OK;
}

static esp_err_t job_fail(void *arg)
{
    (void)arg;
    p3a_job_set_progress(30);
    return P3A_ERR_CORRUPT;
}

static esp_err_t job_slow(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    return ESP_OK;
}

// Writes to flash from a runner task. Runner stacks live in PSRAM, and this
// proves that a flash write from such a task is safe on this chip.
static esp_err_t job_nvs(void *arg)
{
    (void)arg;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    p3a_job_set_progress(25);

    nvs_handle_t nvs;
    err = nvs_open("p3a_test", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t counter = 0;
    nvs_get_u32(nvs, "counter", &counter);
    err = nvs_set_u32(nvs, "counter", counter + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    p3a_job_set_progress(75);
    uint32_t read_back = 0;
    if (err == ESP_OK) {
        err = nvs_get_u32(nvs, "counter", &read_back);
    }
    nvs_close(nvs);
    if (err == ESP_OK && read_back != counter + 1) {
        err = ESP_ERR_INVALID_STATE;
    }
    return err;
}

// !job <ok|fail|slow|nvs> [io|ui]
static void cmd_job(const char *args)
{
    char kind[8] = {0};
    char where[4] = "io";
    if (sscanf(args, "%7s %3s", kind, where) < 1) {
        p3a_test_reply_err("usage: !job <ok|fail|slow|nvs> [io|ui]");
        return;
    }
    p3a_job_fn fn = strcmp(kind, "ok") == 0     ? job_ok
                    : strcmp(kind, "fail") == 0 ? job_fail
                    : strcmp(kind, "slow") == 0 ? job_slow
                    : strcmp(kind, "nvs") == 0  ? job_nvs
                                                : NULL;
    if (!fn) {
        p3a_test_reply_err("unknown job kind");
        return;
    }
    p3a_job_runner_t runner = strcmp(where, "ui") == 0 ? P3A_JOBS_UI : P3A_JOBS_IO;
    char name[P3A_JOB_NAME_MAX];
    snprintf(name, sizeof(name), "test_%s", kind);
    p3a_job_id_t id = P3A_JOB_ID_NONE;
    esp_err_t err = p3a_job_submit(runner, name, fn, NULL, &id);
    if (err != ESP_OK) {
        p3a_test_reply_err(p3a_err_to_name(err));
        return;
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"id\":%u}", (unsigned)id);
    p3a_test_reply_ok(json);
}

// !jobinfo <id>
static void cmd_jobinfo(const char *args)
{
    p3a_job_info_t info;
    if (!p3a_job_get((p3a_job_id_t)strtoul(args, NULL, 0), &info)) {
        p3a_test_reply_err("unknown job");
        return;
    }
    char json[200];
    snprintf(json, sizeof(json),
             "{\"id\":%u,\"name\":\"%s\",\"state\":\"%s\",\"progress\":%u,\"result\":%d,\"result_name\":\"%s\"}",
             (unsigned)info.id, info.name, p3a_job_state_name(info.state), (unsigned)info.progress, (int)info.result,
             p3a_err_to_name(info.result));
    p3a_test_reply_ok(json);
}

// --- events ---------------------------------------------------------------------

static struct {
    atomic_int first;       // TEST_A deliveries to the first subscriber
    atomic_int second;      // ... to the second subscriber
    atomic_int relayed;     // TEST_B deliveries (each emitted from inside a TEST_A handler)
    atomic_int once;        // deliveries to the handler that unsubscribes itself
    atomic_int slow;        // deliveries to the deliberately slow handler
    atomic_int order_errors;
    int32_t last_first;     // touched only on the dispatch task
    int32_t last_second;
    bool second_saw_first;  // the second subscriber ran after the first for the same event
} s_ev;

static void on_a_first(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->payload.i32 != s_ev.last_first + 1) {
        atomic_fetch_add(&s_ev.order_errors, 1);
    }
    s_ev.last_first = event->payload.i32;
    atomic_fetch_add(&s_ev.first, 1);
    p3a_event_emit_i32(P3A_EVENT_TEST_B, event->payload.i32);  // emitting from a handler must be safe
}

static void on_a_second(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_ev.last_first != event->payload.i32) {
        atomic_fetch_add(&s_ev.order_errors, 1);  // subscription order was not respected
    }
    s_ev.last_second = event->payload.i32;
    atomic_fetch_add(&s_ev.second, 1);
}

static void on_a_once(const p3a_event_t *event, void *ctx)
{
    (void)event;
    // Unsubscribing from inside a handler deadlocked in 1.x, where handlers
    // ran with the bus mutex held.
    p3a_event_unsubscribe(P3A_EVENT_TEST_A, on_a_once, ctx);
    atomic_fetch_add(&s_ev.once, 1);
}

static void on_b(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    atomic_fetch_add(&s_ev.relayed, 1);
}

static void on_b_slow(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(20));
    atomic_fetch_add(&s_ev.slow, 1);
}

static void events_reset(void)
{
    p3a_event_unsubscribe(P3A_EVENT_TEST_A, on_a_first, NULL);
    p3a_event_unsubscribe(P3A_EVENT_TEST_A, on_a_second, NULL);
    p3a_event_unsubscribe(P3A_EVENT_TEST_A, on_a_once, NULL);
    p3a_event_unsubscribe(P3A_EVENT_TEST_B, on_b, NULL);
    p3a_event_unsubscribe(P3A_EVENT_TEST_B, on_b_slow, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));  // let anything in flight finish
    memset(&s_ev, 0, sizeof(s_ev));
}

// !event fanout <n>   emits n TEST_A events and reports what the handlers saw.
// !event flood <n>    emits n TEST_B events at a slow handler to fill the queue.
static void cmd_event(const char *args)
{
    char mode[8] = {0};
    int n = 0;
    if (sscanf(args, "%7s %d", mode, &n) != 2 || n < 1 || n > 1000) {
        p3a_test_reply_err("usage: !event <fanout|flood> <1..1000>");
        return;
    }
    events_reset();
    const uint32_t dropped_before = p3a_event_dropped();
    int accepted = 0;
    int expect_slow = 0;

    if (strcmp(mode, "fanout") == 0) {
        p3a_event_subscribe(P3A_EVENT_TEST_A, on_a_first, NULL);
        p3a_event_subscribe(P3A_EVENT_TEST_A, on_a_second, NULL);
        p3a_event_subscribe(P3A_EVENT_TEST_A, on_a_once, NULL);
        p3a_event_subscribe(P3A_EVENT_TEST_B, on_b, NULL);
        for (int i = 1; i <= n; i++) {
            accepted += p3a_event_emit_i32(P3A_EVENT_TEST_A, i) == ESP_OK;
        }
        for (int waited = 0; waited < 2000 && atomic_load(&s_ev.relayed) < accepted; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else if (strcmp(mode, "flood") == 0) {
        p3a_event_subscribe(P3A_EVENT_TEST_B, on_b_slow, NULL);
        for (int i = 0; i < n; i++) {
            accepted += p3a_event_emit(P3A_EVENT_TEST_B) == ESP_OK;
        }
        expect_slow = accepted;
        for (int waited = 0; waited < 5000 && atomic_load(&s_ev.slow) < expect_slow; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        p3a_test_reply_err("unknown mode");
        return;
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"emitted\":%d,\"accepted\":%d,\"first\":%d,\"second\":%d,\"relayed\":%d,\"once\":%d,\"slow\":%d,"
             "\"order_errors\":%d,\"dropped\":%u}",
             n, accepted, atomic_load(&s_ev.first), atomic_load(&s_ev.second), atomic_load(&s_ev.relayed),
             atomic_load(&s_ev.once), atomic_load(&s_ev.slow), atomic_load(&s_ev.order_errors),
             (unsigned)(p3a_event_dropped() - dropped_before));
    events_reset();
    p3a_test_reply_ok(json);
}

// --- memory pressure ------------------------------------------------------------

#define HOG_CHUNK_BYTES 2048
#define HOG_MAX_CHUNKS  320  // 640 KB: more than the whole pool

static void *s_hog[HOG_MAX_CHUNKS];
static int s_hog_count;
static atomic_int s_pressure_events;
static atomic_int s_pressure_last = -1;
static bool s_pressure_subscribed;

static void on_pressure(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    atomic_store(&s_pressure_last, event->payload.i32);
    atomic_fetch_add(&s_pressure_events, 1);
}

static void reply_mem(void)
{
    vTaskDelay(pdMS_TO_TICKS(30));  // let a pressure event, if any, be delivered
    p3a_mem_sample_t s = p3a_mem_sample();
    char json[256];
    snprintf(json, sizeof(json),
             "{\"level\":\"%s\",\"free\":%u,\"largest\":%u,\"min_free\":%u,\"total\":%u,\"hogged\":%d,"
             "\"events\":%d,\"last_event_level\":%d}",
             p3a_mem_level_name(s.level), (unsigned)s.free_bytes, (unsigned)s.largest_block,
             (unsigned)s.min_free_bytes, (unsigned)s.total_bytes, s_hog_count * HOG_CHUNK_BYTES,
             atomic_load(&s_pressure_events), atomic_load(&s_pressure_last));
    p3a_test_reply_ok(json);
}

// !mem                 samples the DMA pool and reports it
// !mem hog <bytes>     takes that much from the pool, in 2 KB chunks, and holds it
// !mem release         gives it all back
static void cmd_mem(const char *args)
{
    if (!s_pressure_subscribed) {
        s_pressure_subscribed = p3a_event_subscribe(P3A_EVENT_MEM_PRESSURE_CHANGED, on_pressure, NULL) == ESP_OK;
    }
    if (strncmp(args, "hog ", 4) == 0) {
        long want = strtol(args + 4, NULL, 0) / HOG_CHUNK_BYTES;
        while (want-- > 0 && s_hog_count < HOG_MAX_CHUNKS) {
            void *chunk = heap_caps_malloc(HOG_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            if (!chunk) {
                break;
            }
            s_hog[s_hog_count++] = chunk;
        }
    } else if (strcmp(args, "release") == 0) {
        while (s_hog_count > 0) {
            heap_caps_free(s_hog[--s_hog_count]);
        }
    } else if (args[0] != '\0') {
        p3a_test_reply_err("usage: !mem [hog <bytes>|release]");
        return;
    }
    p3a_mem_sample();  // evaluate now, so that a change of level emits its event
    reply_mem();
}

const p3a_test_command_t p3a_test_util_commands[] = {
    {"task", cmd_task},     {"errname", cmd_errname}, {"job", cmd_job},
    {"jobinfo", cmd_jobinfo}, {"event", cmd_event},     {"mem", cmd_mem},
};
const size_t p3a_test_util_command_count = sizeof(p3a_test_util_commands) / sizeof(p3a_test_util_commands[0]);
