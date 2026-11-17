// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file play_scheduler_switch.c
 * @brief Asynchronous playset switching for Play Scheduler
 *
 * play_scheduler_execute_playset() takes seconds for large playsets (~3 SD
 * file operations per channel, all under s_state->mutex). Running it inside
 * the single httpd task froze every HTTP endpoint — including the web UI's
 * 4 s /playsets/active poll — for the whole switch. This module moves the
 * execution to a dedicated worker task:
 *
 *   - Callers enqueue a request and return immediately. The HTTP layer
 *     responds with {"switching":true} and the web UI converges via its
 *     existing /playsets/active poll.
 *   - Single-slot, latest-wins coalescing: a new request supersedes a
 *     queued-but-not-started one (its playset buffer is freed). Rapid pill
 *     clicks collapse to at most one executing + one queued request.
 *   - Completion/failure is surfaced through play_scheduler_get_switch_status()
 *     (switch_seq increments per completed attempt; last_error_code carries
 *     the failure). The getter never touches s_state->mutex, so the status
 *     endpoint stays responsive while a switch holds it.
 *
 * Two request flavors:
 *   - BY_VALUE: the caller provides a fully-resolved playset (built-in spec
 *     or loaded from the SD library). Ownership of the heap buffer transfers
 *     to this module on successful enqueue.
 *   - BY_NAME: the worker resolves the name itself — SD library first, then
 *     a Makapix server fetch (up to ~30 s of MQTT retries, previously borne
 *     by the httpd task).
 *
 * Boot restore and the single-channel play_scheduler_play_* wrappers keep
 * calling execute_playset synchronously; both paths are fast and/or have no
 * client waiting. Concurrent sync executes serialize against the worker on
 * s_state->mutex exactly as two sync callers do today (last completer wins).
 */

#include "play_scheduler_internal.h"
#include "play_scheduler.h"
#include "playset_store.h"
#include "makapix_api.h"
#include "makapix_mqtt.h"
#include "psram_alloc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "ps_switch";

// Same stack budget as the refresh task: the worker calls into FATFS via
// execute_playset's per-channel cache loads (the same call previously ran on
// the 8 KB httpd task stack).
#define SWITCH_TASK_STACK_SIZE 8192

// Event bits (mirrors play_scheduler_refresh.c)
#define SWITCH_EVENT_WORK      (1 << 0)
#define SWITCH_EVENT_SHUTDOWN  (1 << 1)
#define SWITCH_EVENT_PARKED    (1 << 2)  // Set by task immediately before vTaskSuspend(NULL)

// Optional slave-OTA gate. Weak so play_scheduler doesn't grow a build
// dependency on slave_ota (same pattern as the app_lcd/ugfx_ui weak refs in
// play_scheduler_playsets.c). HTTP/MQTT entry points gate requests before
// enqueueing; this is the defense-in-depth re-check at execute time.
extern bool slave_ota_is_in_progress(void) __attribute__((weak));

typedef enum {
    PS_SWITCH_BY_VALUE,   // request carries a resolved playset (slot owns it)
    PS_SWITCH_BY_NAME,    // worker resolves the name (library, then server)
} ps_switch_kind_t;

typedef struct {
    bool occupied;
    ps_switch_kind_t kind;
    char name[PS_PLAYSET_NAME_MAX + 1];
    ps_playset_t *playset;   // BY_VALUE: owned by the slot; BY_NAME: NULL
} ps_switch_slot_t;

// s_switch_mutex guards s_slot AND s_status together. Critical sections are
// struct copies only — never held across execute_playset or the MQTT fetch —
// so contention is microseconds. A single lock makes the enqueue/complete
// interleaving trivially atomic (no window where a queued request is visible
// in the slot but not yet reflected in status, or vice versa).
static ps_switch_slot_t   s_slot;
static ps_switch_status_t s_status;
static SemaphoreHandle_t  s_switch_mutex = NULL;

static EventGroupHandle_t s_switch_events = NULL;
static TaskHandle_t       s_switch_task = NULL;
static StackType_t       *s_switch_stack = NULL;
static StaticTask_t       s_switch_task_buffer;
static volatile bool      s_task_running = false;

// ============================================================================
// Worker
// ============================================================================

/**
 * Run one switch attempt to completion. Returns the status code string
 * ("" on success) — the caller folds it into s_status.
 */
static const char *switch_attempt(const ps_switch_slot_t *req)
{
    const char *code = "";
    ps_playset_t *ps = req->playset;

    if (slave_ota_is_in_progress && slave_ota_is_in_progress()) {
        // Entry points already gate on this; re-check because the OTA may
        // have started while the request sat queued behind another switch.
        code = "OTA_IN_PROGRESS";
        goto done;
    }

    if (req->kind == PS_SWITCH_BY_NAME) {
        ps = psram_calloc(1, sizeof(ps_playset_t));
        if (!ps) {
            code = "OOM";
            goto done;
        }
        if (playset_store_load(req->name, ps) == ESP_OK) {
            // Cached locally (possibly since the request was enqueued).
        } else if (makapix_mqtt_is_connected()) {
            // Server fetch: up to ~30 s (10 s timeout × 3 retries). This is
            // exactly the wait the async path exists to keep off the httpd
            // task; status reads "switching" for its whole duration.
            esp_err_t err = makapix_api_get_playset(req->name, ps);
            if (err == ESP_OK) {
                strlcpy(ps->name, req->name, sizeof(ps->name));
                esp_err_t save_err = playset_store_save(req->name, ps);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to cache playset '%s': %s",
                             req->name, esp_err_to_name(save_err));
                }
            } else {
                code = (err == ESP_ERR_TIMEOUT) ? "MQTT_TIMEOUT"
                                                : "PLAYSET_NOT_FOUND";
                goto done;
            }
        } else {
            code = "NOT_CONNECTED";
            goto done;
        }
    }

    if (ps) {
        // All async requests originate from explicit user actions (web UI
        // click, MQTT command), so the pick failure mode is LOUD.
        esp_err_t err = play_scheduler_execute_playset(ps, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "execute_playset('%s') failed: %s",
                     req->name, esp_err_to_name(err));
            code = "EXECUTE_ERROR";
        }
    } else {
        // BY_VALUE with a NULL playset is rejected at enqueue; defensive.
        code = "EXECUTE_ERROR";
    }

done:
    if (ps) free(ps);
    return code;
}

static void switch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Switch task started");

    while (s_task_running) {
        EventBits_t bits = xEventGroupWaitBits(
            s_switch_events,
            SWITCH_EVENT_WORK | SWITCH_EVENT_SHUTDOWN,
            pdTRUE,   // clear on exit
            pdFALSE,  // wait for any
            portMAX_DELAY);

        if (bits & SWITCH_EVENT_SHUTDOWN) {
            break;
        }

        // Drain the slot until empty: a request enqueued while we were
        // executing is picked up here without waiting for another WORK bit
        // (the bit may have been consumed by the wait above already).
        for (;;) {
            ps_switch_slot_t req;

            xSemaphoreTake(s_switch_mutex, portMAX_DELAY);
            if (!s_slot.occupied) {
                xSemaphoreGive(s_switch_mutex);
                break;
            }
            req = s_slot;                 // worker now owns req.playset
            s_slot.occupied = false;
            s_slot.playset = NULL;
            // Idempotent status refresh: enqueue already set these, but a
            // superseding enqueue may have changed pending_name since.
            s_status.switching = true;
            strlcpy(s_status.pending_name, req.name, sizeof(s_status.pending_name));
            xSemaphoreGive(s_switch_mutex);

            ESP_LOGI(TAG, "Switching to playset '%s' (%s)", req.name,
                     req.kind == PS_SWITCH_BY_NAME ? "by name" : "by value");

            const char *code = switch_attempt(&req);

            xSemaphoreTake(s_switch_mutex, portMAX_DELAY);
            s_status.switch_seq++;
            strlcpy(s_status.last_error_code, code, sizeof(s_status.last_error_code));
            if (s_slot.occupied) {
                // A newer request arrived mid-execute: stay "switching" and
                // point at it so the UI never sees a false settled state.
                s_status.switching = true;
                strlcpy(s_status.pending_name, s_slot.name, sizeof(s_status.pending_name));
            } else {
                s_status.switching = false;
                s_status.pending_name[0] = '\0';
            }
            xSemaphoreGive(s_switch_mutex);

            if (code[0] != '\0') {
                ESP_LOGW(TAG, "Switch to '%s' failed: %s", req.name, code);
            } else {
                ESP_LOGI(TAG, "Switch to '%s' complete", req.name);
            }
        }
    }

    ESP_LOGI(TAG, "Switch task exiting");
    // Park at a sync point so ps_switch_stop can delete us externally (same
    // static-allocation teardown protocol as the refresh task — see the
    // comment in play_scheduler_refresh.c).
    if (s_switch_events) {
        xEventGroupSetBits(s_switch_events, SWITCH_EVENT_PARKED);
    }
    vTaskSuspend(NULL);
    // Defensive: never reached.
    vTaskDelete(NULL);
}

// ============================================================================
// Enqueue (internal)
// ============================================================================

static esp_err_t switch_enqueue(ps_switch_kind_t kind, const char *name,
                                ps_playset_t *playset)
{
    if (!name || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_switch_task == NULL || s_switch_mutex == NULL || s_switch_events == NULL) {
        // Worker never started (init failure) — callers fall back or 503.
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_switch_mutex, portMAX_DELAY);

    // Latest-wins: supersede a queued-but-not-started request. The worker
    // NULLs s_slot.playset when it takes ownership, so a non-NULL pointer
    // here is always an un-started buffer that only we reference.
    if (s_slot.occupied && s_slot.playset) {
        ESP_LOGI(TAG, "Superseding queued switch to '%s'", s_slot.name);
        free(s_slot.playset);
    }
    s_slot.occupied = true;
    s_slot.kind = kind;
    strlcpy(s_slot.name, name, sizeof(s_slot.name));
    s_slot.playset = playset;

    s_status.switching = true;
    strlcpy(s_status.pending_name, name, sizeof(s_status.pending_name));

    xSemaphoreGive(s_switch_mutex);

    xEventGroupSetBits(s_switch_events, SWITCH_EVENT_WORK);
    return ESP_OK;
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t play_scheduler_request_switch_by_value(ps_playset_t *playset, const char *name)
{
    if (!playset || playset->channel_count == 0 || playset->channel_count > PS_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    // Status/display name: prefer the caller's (the name the UI knows, e.g.
    // "channel_recent"), falling back to the playset's own. By-value requests
    // are fully resolved, so the name is display-only — anonymous playsets
    // (e.g. an MQTT execute_playset command without a "name" field) get a
    // placeholder rather than a rejection.
    const char *display = (name && name[0] != '\0') ? name : playset->name;
    if (display[0] == '\0') display = "(unnamed)";
    return switch_enqueue(PS_SWITCH_BY_VALUE, display, playset);
}

esp_err_t play_scheduler_request_switch_by_name(const char *name)
{
    return switch_enqueue(PS_SWITCH_BY_NAME, name, NULL);
}

void play_scheduler_get_switch_status(ps_switch_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s_switch_mutex == NULL) {
        // Worker never started: report the permanent not-switching state.
        return;
    }
    xSemaphoreTake(s_switch_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_switch_mutex);
}

// ============================================================================
// Lifecycle (mirrors ps_refresh_start/stop)
// ============================================================================

esp_err_t ps_switch_start(void)
{
    if (s_switch_task != NULL) {
        ESP_LOGD(TAG, "Switch task already running");
        return ESP_OK;
    }

    if (s_switch_mutex == NULL) {
        s_switch_mutex = xSemaphoreCreateMutex();
        if (s_switch_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_switch_events == NULL) {
        s_switch_events = xEventGroupCreate();
        if (s_switch_events == NULL) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_slot, 0, sizeof(s_slot));
    memset(&s_status, 0, sizeof(s_status));
    s_task_running = true;

    // Create switch task with SPIRAM-backed stack
    if (!s_switch_stack) {
        s_switch_stack = heap_caps_malloc(SWITCH_TASK_STACK_SIZE * sizeof(StackType_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    // Pin to Core 0 to avoid interfering with animation rendering on Core 1
    bool task_created = false;
    if (s_switch_stack) {
        s_switch_task = xTaskCreateStaticPinnedToCore(switch_task, "ps_switch",
                                            SWITCH_TASK_STACK_SIZE, NULL, CONFIG_P3A_APP_TASK_PRIORITY,
                                            s_switch_stack, &s_switch_task_buffer, 0);
        task_created = (s_switch_task != NULL);
    }

    if (!task_created) {
        if (xTaskCreatePinnedToCore(switch_task, "ps_switch",
                        SWITCH_TASK_STACK_SIZE, NULL, CONFIG_P3A_APP_TASK_PRIORITY, &s_switch_task, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create switch task");
            s_task_running = false;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Switch task created");
    return ESP_OK;
}

void ps_switch_stop(void)
{
    if (s_switch_task == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Stopping switch task");
    s_task_running = false;

    if (s_switch_events) {
        xEventGroupSetBits(s_switch_events, SWITCH_EVENT_SHUTDOWN);
    }

    TaskHandle_t handle = s_switch_task;

    // Wait for the task to reach the parked sync point. The task sets
    // SWITCH_EVENT_PARKED immediately before vTaskSuspend(NULL). The wait
    // covers a worst-case in-flight execute (multi-second cache loads).
    bool parked = false;
    if (s_switch_events) {
        EventBits_t bits = xEventGroupWaitBits(s_switch_events, SWITCH_EVENT_PARKED,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
        parked = (bits & SWITCH_EVENT_PARKED) != 0;
    }

    if (!parked) {
        ESP_LOGE(TAG, "ps_switch task did not park within 15s; leaking task to preserve memory safety");
        // Leave s_switch_task non-NULL so a future ps_switch_start sees it
        // and skips creating a duplicate.
        return;
    }

    // Close the gap between event-set and vTaskSuspend(NULL).
    for (int i = 0; i < 100 && eTaskGetState(handle) != eSuspended; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    s_switch_task = NULL;
    vTaskDelete(handle);

    // Let the idle task reap the deleted TCB before the static buffer can be
    // safely reused (see ps_refresh_stop for the full rationale).
    vTaskDelay(pdMS_TO_TICKS(20));

    // Free a residual queued request that the worker never picked up.
    if (s_switch_mutex) {
        xSemaphoreTake(s_switch_mutex, portMAX_DELAY);
        if (s_slot.playset) {
            free(s_slot.playset);
        }
        memset(&s_slot, 0, sizeof(s_slot));
        s_status.switching = false;
        s_status.pending_name[0] = '\0';
        xSemaphoreGive(s_switch_mutex);
    }

    if (s_switch_events) {
        vEventGroupDelete(s_switch_events);
        s_switch_events = NULL;
    }
}
