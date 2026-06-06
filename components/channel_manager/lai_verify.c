// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file lai_verify.c
 * @brief LAi verification sweep: paced reconciliation of LAi against disk
 *
 * Implements the sweep engine declared in lai_verify.h. Architecture:
 *
 * - Requests (from the play scheduler's pick path) land in a per-channel
 *   slot table under a small leaf mutex; a volatile pending counter makes
 *   lai_verify_has_work() lock-free for the download task's hot loop.
 * - All verification I/O runs in the download manager task, one batch per
 *   loop iteration, preserving the single-background-SD-scanner pattern.
 * - The sweep operates on an owned PSRAM snapshot of the channel's LAi
 *   post_id array taken at sweep start. Plain int32 copies are immune to
 *   concurrent cache merges, evictions, and frees; per-entry
 *   lai_remove_entry() is keyed by post_id and idempotent, so concurrent
 *   download completions re-adding a just-checked entry are correct by
 *   construction.
 * - The channel's cache pointer is re-resolved from the registry under
 *   channel_cache_lifecycle_lock() on EVERY batch. A NULL result (playset
 *   switched, cache freed) aborts the sweep — this is the abort mechanism.
 *
 * Lock ordering rules:
 * - s_verify_mutex is a leaf: never call out of this module while holding it.
 * - Never take the play scheduler's state mutex while holding the cache
 *   lifecycle lock (execute_playset holds them in the opposite order).
 *   Cursor compensation is therefore buffered inside the batch and applied
 *   after the lifecycle lock is released.
 */

#include "lai_verify.h"
#include "channel_cache.h"
#include "download_manager.h"
#include "makapix_channel_events.h"
#include "play_scheduler.h"
#include "psram_alloc.h"
#include "sdio_bus.h"
#include "p3a_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "lai_verify";

// External: SD paused check (weak, may be absent in some builds)
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

// Stats per batch; one batch runs per download-task loop iteration.
#define LAI_VERIFY_BATCH_STATS    16
// Pacing delay after every batch that touched the SD card.
#define LAI_VERIFY_BATCH_DELAY_MS 50
// Per-channel cooldown after a completed sweep; requests inside the window
// are dropped (the pick path's miss window must re-accumulate anyway).
#define LAI_VERIFY_COOLDOWN_MS    120000
// Abort the active sweep after this many consecutive errored batches
// (FS-level failures that the SD-availability gate didn't catch).
#define LAI_VERIFY_MAX_ERROR_STREAK 10

// ============================================================================
// Request slots
// ============================================================================

typedef struct {
    char channel_id[64];        // Empty = free slot
    bool pending;               // Sweep requested, not yet started
    bool swept_once;            // last_sweep_tick is valid
    TickType_t last_sweep_tick; // Tick of last completed sweep (cooldown)
} lai_verify_slot_t;

static lai_verify_slot_t s_slots[PS_MAX_CHANNELS];
static SemaphoreHandle_t s_verify_mutex = NULL;

// Lock-free mirror of "any slot pending" for the download task's hot loop.
// Single-word writes are atomic on ESP32; updated only under s_verify_mutex.
static volatile int s_pending_count = 0;

// ============================================================================
// Active sweep state (touched only by the download task)
// ============================================================================

typedef struct {
    bool active;
    char channel_id[64];
    char display_name[64];
    int32_t *snapshot;          // Owned PSRAM copy of available_post_ids
    size_t snapshot_count;
    size_t cursor;              // Next snapshot index to verify
    size_t checked;             // Files confirmed present
    size_t evicted;             // LAi entries removed (missing / stale)
    size_t skipped;             // Sentinels / path-build failures (not verified)
    int error_streak;           // Consecutive batches that hit FS errors
    TickType_t start_tick;
} lai_sweep_state_t;

static lai_sweep_state_t s_sweep;

// ============================================================================
// Lifecycle
// ============================================================================

void lai_verify_init(void)
{
    if (s_verify_mutex) {
        return;
    }
    s_verify_mutex = xSemaphoreCreateMutex();
    if (!s_verify_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
}

// ============================================================================
// Requests
// ============================================================================

void lai_verify_request(const char *channel_id)
{
    if (!channel_id || channel_id[0] == '\0' || !s_verify_mutex) {
        return;
    }

    bool queued = false;

    if (xSemaphoreTake(s_verify_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        // Caller may hold the scheduler mutex — never block long here. The
        // miss evidence persists, so a dropped request re-fires shortly.
        ESP_LOGD(TAG, "Request dropped (mutex busy): %.16s", channel_id);
        return;
    }

    // Find this channel's slot, or a free one, or evict the stalest idle one.
    int slot = -1;
    int free_slot = -1;
    int evict_slot = -1;
    for (int i = 0; i < PS_MAX_CHANNELS; i++) {
        if (s_slots[i].channel_id[0] == '\0') {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(s_slots[i].channel_id, channel_id) == 0) {
            slot = i;
            break;
        }
        if (!s_slots[i].pending &&
            (evict_slot < 0 ||
             (TickType_t)(s_slots[i].last_sweep_tick - s_slots[evict_slot].last_sweep_tick) > (TickType_t)0x80000000u)) {
            evict_slot = i;  // oldest non-pending slot (wrap-safe compare)
        }
    }
    if (slot < 0) slot = free_slot;
    if (slot < 0) slot = evict_slot;

    if (slot >= 0) {
        lai_verify_slot_t *s = &s_slots[slot];
        if (strcmp(s->channel_id, channel_id) != 0) {
            // Claiming a free/evicted slot for a new channel
            strlcpy(s->channel_id, channel_id, sizeof(s->channel_id));
            s->pending = false;
            s->swept_once = false;
            s->last_sweep_tick = 0;
        }
        if (!s->pending) {
            bool in_cooldown = s->swept_once &&
                (xTaskGetTickCount() - s->last_sweep_tick) < pdMS_TO_TICKS(LAI_VERIFY_COOLDOWN_MS);
            if (in_cooldown) {
                ESP_LOGD(TAG, "Request within cooldown, dropped: %.16s", channel_id);
            } else {
                s->pending = true;
                s_pending_count++;
                queued = true;
            }
        }
    }

    xSemaphoreGive(s_verify_mutex);

    if (queued) {
        ESP_LOGI(TAG, "Sweep queued for channel %.16s", channel_id);
        // Wake the download task (it hosts the sweep batches)
        makapix_channel_signal_downloads_needed();
    }
}

bool lai_verify_has_work(void)
{
    return s_sweep.active || s_pending_count > 0;
}

// ============================================================================
// Sweep execution (download task only)
// ============================================================================

/**
 * @brief Pop the next pending slot into s_sweep (without snapshotting yet)
 *
 * @return true if a sweep was started
 */
static bool sweep_start_next(void)
{
    if (!s_verify_mutex) return false;

    bool started = false;
    if (xSemaphoreTake(s_verify_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < PS_MAX_CHANNELS; i++) {
            if (!s_slots[i].pending) continue;
            s_slots[i].pending = false;
            if (s_pending_count > 0) s_pending_count--;

            // Enforce the cooldown here too: a request that arrived while
            // this same channel was actively being swept would otherwise
            // re-sweep immediately on completion.
            if (s_slots[i].swept_once &&
                (xTaskGetTickCount() - s_slots[i].last_sweep_tick) < pdMS_TO_TICKS(LAI_VERIFY_COOLDOWN_MS)) {
                ESP_LOGD(TAG, "Queued sweep within cooldown, dropped: %.16s",
                         s_slots[i].channel_id);
                continue;
            }

            memset(&s_sweep, 0, sizeof(s_sweep));
            strlcpy(s_sweep.channel_id, s_slots[i].channel_id, sizeof(s_sweep.channel_id));
            s_sweep.active = true;
            s_sweep.start_tick = xTaskGetTickCount();
            started = true;
            break;
        }
        xSemaphoreGive(s_verify_mutex);
    }
    return started;
}

/**
 * @brief Stamp the cooldown tick for a completed sweep and clear sweep state
 *
 * @param completed true if the sweep ran to the end of its snapshot (stamps
 *                  the cooldown); false for aborts (channel gone, no memory)
 */
static void sweep_finish(bool completed)
{
    if (s_sweep.snapshot) {
        free(s_sweep.snapshot);
        s_sweep.snapshot = NULL;
    }

    if (completed && s_verify_mutex &&
        xSemaphoreTake(s_verify_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < PS_MAX_CHANNELS; i++) {
            if (strcmp(s_slots[i].channel_id, s_sweep.channel_id) == 0) {
                s_slots[i].swept_once = true;
                s_slots[i].last_sweep_tick = xTaskGetTickCount();
                break;
            }
        }
        xSemaphoreGive(s_verify_mutex);
    }

    s_sweep.active = false;
}

lai_verify_result_t lai_verify_run_batch(void)
{
    if (!lai_verify_has_work()) {
        return LAI_VERIFY_IDLE;
    }

    // Gates: only verify against a trustworthy, uncontended filesystem.
    // Work stays queued; the download loop polls back while it remains.
    if (!makapix_channel_is_sd_available() ||
        sdio_bus_is_locked() ||
        (animation_player_is_sd_paused && animation_player_is_sd_paused()) ||
        p3a_state_get() == P3A_STATE_PICO8_STREAMING) {
        return LAI_VERIFY_GATED;
    }

    if (!s_sweep.active && !sweep_start_next()) {
        return LAI_VERIFY_IDLE;
    }

    // Re-resolve the cache fresh each batch: a playset switch frees caches,
    // and this registry lookup under the lifecycle lock is the abort path.
    channel_cache_lifecycle_lock();
    channel_cache_t *cache = channel_cache_registry_find(s_sweep.channel_id);
    if (!cache) {
        channel_cache_lifecycle_unlock();
        ESP_LOGI(TAG, "Sweep aborted: channel %.16s no longer active", s_sweep.channel_id);
        sweep_finish(false);
        return LAI_VERIFY_RAN;
    }

    // First batch: snapshot the LAi post_id array (owned PSRAM copy).
    if (!s_sweep.snapshot) {
        xSemaphoreTake(cache->mutex, portMAX_DELAY);
        size_t count = cache->available_count;
        if (count == 0) {
            xSemaphoreGive(cache->mutex);
            ESP_LOGI(TAG, "Sweep skipped: '%s' LAi is empty", cache->display_name);
            channel_cache_lifecycle_unlock();
            char ch_id[64];
            strlcpy(ch_id, s_sweep.channel_id, sizeof(ch_id));
            sweep_finish(true);
            play_scheduler_on_lai_swept(ch_id, 0, 0);
            return LAI_VERIFY_RAN;
        }
        s_sweep.snapshot = psram_malloc(count * sizeof(int32_t));
        if (!s_sweep.snapshot) {
            xSemaphoreGive(cache->mutex);
            channel_cache_lifecycle_unlock();
            ESP_LOGE(TAG, "Sweep aborted: no memory for %zu-entry snapshot", count);
            sweep_finish(false);
            return LAI_VERIFY_RAN;
        }
        memcpy(s_sweep.snapshot, cache->available_post_ids, count * sizeof(int32_t));
        s_sweep.snapshot_count = count;
        strlcpy(s_sweep.display_name, cache->display_name, sizeof(s_sweep.display_name));
        xSemaphoreGive(cache->mutex);
        ESP_LOGI(TAG, "Sweep started: '%s' (%zu LAi entries)",
                 s_sweep.display_name, s_sweep.snapshot_count);
    }

    // Buffered cursor compensation: positions are applied after the
    // lifecycle lock is released (lock ordering, see file header).
    int removed_pos[LAI_VERIFY_BATCH_STATS];
    size_t removed_n = 0;
    bool io_done = false;
    bool fs_error = false;

    size_t batch_end = s_sweep.cursor + LAI_VERIFY_BATCH_STATS;
    if (batch_end > s_sweep.snapshot_count) {
        batch_end = s_sweep.snapshot_count;
    }

    static char s_verify_path[256];  // download task only

    while (s_sweep.cursor < batch_end) {
        int32_t post_id = s_sweep.snapshot[s_sweep.cursor];
        s_sweep.cursor++;

        // Copy the Ci entry under the cache mutex (merges can reallocate
        // the entry array and rebuild the hash while we read).
        makapix_channel_entry_t entry;
        bool in_ci = false;
        xSemaphoreTake(cache->mutex, portMAX_DELAY);
        uint32_t ci_index = ci_find_by_post_id(cache, post_id);
        if (ci_index != UINT32_MAX && cache->entries && ci_index < cache->entry_count) {
            entry = cache->entries[ci_index];
            in_ci = true;
        }
        xSemaphoreGive(cache->mutex);

        if (!in_ci) {
            // LAi entry with no Ci backing — same stale-entry eviction the
            // pickers perform on hash misses.
            int pos = -1;
            if (lai_remove_entry(cache, post_id, &pos)) {
                s_sweep.evicted++;
                if (pos >= 0 && removed_n < LAI_VERIFY_BATCH_STATS) {
                    removed_pos[removed_n++] = pos;
                }
            }
            continue;
        }

        esp_err_t perr = download_manager_build_entry_filepath(
            s_sweep.channel_id, &entry, s_verify_path, sizeof(s_verify_path));
        if (perr == ESP_ERR_NOT_SUPPORTED) {
            // Institution sentinel (no downloadable file by design); never
            // in LAi normally — skip defensively.
            s_sweep.skipped++;
            continue;
        }
        if (perr != ESP_OK || s_verify_path[0] == '\0') {
            // Path infrastructure failure — not evidence about the file.
            s_sweep.skipped++;
            continue;
        }

        struct stat st;
        io_done = true;
        if (stat(s_verify_path, &st) == 0) {
            s_sweep.checked++;
            continue;
        }
        if (errno != ENOENT || !makapix_channel_is_sd_available()) {
            // FS-level error, or the SD vanished mid-batch: rewind this
            // entry and retry next batch.
            s_sweep.cursor--;
            fs_error = true;
            break;
        }

        // Confirmed missing: evict from LAi so the download manager can
        // re-discover (and re-download) it.
        int pos = -1;
        if (lai_remove_entry(cache, post_id, &pos)) {
            s_sweep.evicted++;
            if (pos >= 0 && removed_n < LAI_VERIFY_BATCH_STATS) {
                removed_pos[removed_n++] = pos;
            }
        }
    }

    bool done = !fs_error && (s_sweep.cursor >= s_sweep.snapshot_count);
    if (done && s_sweep.evicted > 0) {
        channel_cache_schedule_save(cache);
    }
    channel_cache_lifecycle_unlock();

    // Cursor compensation outside the lifecycle lock. The cache pointer is
    // only pointer-compared inside (never dereferenced), so this is safe
    // even if the playset switches between unlock and these calls.
    for (size_t i = 0; i < removed_n; i++) {
        play_scheduler_compensate_cursor_after_lai_remove(cache, removed_pos[i]);
    }

    if (fs_error) {
        s_sweep.error_streak++;
        if (s_sweep.error_streak >= LAI_VERIFY_MAX_ERROR_STREAK) {
            ESP_LOGW(TAG, "Sweep aborted after %d errored batches: '%s' (checked=%zu evicted=%zu of %zu)",
                     s_sweep.error_streak, s_sweep.display_name,
                     s_sweep.checked, s_sweep.evicted, s_sweep.snapshot_count);
            // Stamp the cooldown anyway so a broken filesystem can't cause
            // an immediate re-request loop.
            sweep_finish(true);
        }
    } else {
        s_sweep.error_streak = 0;
    }

    if (done) {
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - s_sweep.start_tick) * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Sweep complete: '%s' checked=%zu evicted=%zu skipped=%zu of %zu in %lu ms",
                 s_sweep.display_name, s_sweep.checked, s_sweep.evicted,
                 s_sweep.skipped, s_sweep.snapshot_count, (unsigned long)elapsed_ms);

        char ch_id[64];
        strlcpy(ch_id, s_sweep.channel_id, sizeof(ch_id));
        size_t checked = s_sweep.checked;
        size_t evicted = s_sweep.evicted;

        sweep_finish(true);

        // Notify scheduler (clears miss window, recalcs SWRR weights) and
        // let the download manager re-discover the evicted entries. Both
        // calls take their own locks — we hold none here.
        play_scheduler_on_lai_swept(ch_id, checked, evicted);
        if (evicted > 0) {
            download_manager_rescan();
        }
    }

    if (io_done) {
        vTaskDelay(pdMS_TO_TICKS(LAI_VERIFY_BATCH_DELAY_MS));
    }

    return LAI_VERIFY_RAN;
}
