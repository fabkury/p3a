// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file animation_player_loader.c
 * @brief Background animation file loader with corruption safeguards and retry logic
 */

#include "animation_player_priv.h"
#include "animation_player.h"
#include "esp_heap_caps.h"
#include "play_scheduler.h"
#include "playback_queue.h"
#include "sdio_bus.h"
#include "ota_manager.h"
#include "p3a_render.h"
#include "p3a_state.h"
#include "loader_service.h"
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>
#include "freertos/task.h"
#include "sntp_sync.h"
#include "play_scheduler_internal.h"
#include "channel_cache.h"
#include "download_manager.h"
#include "event_bus.h"

// Some tooling configurations may not resolve component include paths reliably for C files.
// Keep explicit prototypes here to avoid "implicit declaration" diagnostics.
bool sdio_bus_is_locked(void);
const char *sdio_bus_get_holder(void);
bool ota_manager_is_checking(void);

// Processing notification (from display_renderer_priv.h via weak symbol).
// All terminal swap-pipeline sites must signal proc_notif_fail_if_processing
// on failure or proc_notif_success on success so the indicator doesn't get
// stuck blue until the watchdog fires.
extern void proc_notif_success(void) __attribute__((weak));
extern void proc_notif_fail_if_processing(void) __attribute__((weak));

// ============================================================================
// Corrupt file deletion safeguard
// ============================================================================
//
// SAFEGUARD MEASURE: This mechanism prevents accidental cascade deletion of
// good files due to transient errors. It tracks the last time a cached file
// (vault or giphy) was deleted due to corruption and only allows one deletion
// every 60 seconds.
//
// After successful deletion, the entry is also evicted from LAi so the
// scheduler doesn't re-pick the missing file before the next channel refresh.
//
// ============================================================================

// Track last time a corrupt file was deleted (milliseconds since boot)
static uint64_t s_last_corrupt_deletion_ms = 0;
static const uint64_t CORRUPT_DELETION_COOLDOWN_MS = 60000ULL;  // 60 seconds

// ============================================================================
// Silent auto-swap retry burst
// ============================================================================
//
// When an auto-swap (timer-driven, touch nav, channel switch) fails to load,
// we silently pick another artwork up to MAX_AUTO_RETRIES times before giving
// up loudly with an on-screen error. Files that fail during a burst are
// remembered in a small blocklist so the picker doesn't waste retry attempts
// on the same broken file. The state is reset on:
//   - a successful swap (render task, after the back/front buffer flip)
//   - a user-initiated swap request taking over
//   - the burst exhausting its retry budget
//
// User-initiated swaps (HTTP play_artwork / play_local_file) bypass this
// mechanism entirely: the user is asking for a specific artwork, so a
// failure is reported loudly instead of silently swapping past it.
// ============================================================================

#define MAX_AUTO_RETRIES        3
#define AUTO_BLOCKLIST_SIZE     MAX_AUTO_RETRIES
#define MAX_BLOCKLIST_SKIPS     6  // safety: cap consecutive blocklisted picks

static int s_auto_retry_count = 0;
static int s_auto_retry_skip_count = 0;
static int32_t s_auto_retry_blocklist[AUTO_BLOCKLIST_SIZE];
static int s_auto_retry_blocklist_count = 0;

static bool auto_retry_is_blocklisted(int32_t post_id)
{
    if (post_id == 0) {
        return false;
    }
    for (int i = 0; i < s_auto_retry_blocklist_count; i++) {
        if (s_auto_retry_blocklist[i] == post_id) {
            return true;
        }
    }
    return false;
}

static void auto_retry_blocklist_add(int32_t post_id)
{
    if (post_id == 0 || auto_retry_is_blocklisted(post_id)) {
        return;
    }
    if (s_auto_retry_blocklist_count < AUTO_BLOCKLIST_SIZE) {
        s_auto_retry_blocklist[s_auto_retry_blocklist_count++] = post_id;
    }
}

void animation_loader_reset_auto_retry_state(void)
{
    s_auto_retry_count = 0;
    s_auto_retry_skip_count = 0;
    s_auto_retry_blocklist_count = 0;
}

// Extract the filename portion of a path. Returns "(unknown)" for NULL input.
static const char *basename_of(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') {
        return "(unknown)";
    }
    const char *slash = strrchr(filepath, '/');
    return slash ? slash + 1 : filepath;
}

static void show_load_error_message(esp_err_t error, const char *filepath)
{
    // Two-line body: filename on top, error code below. The renderer splits
    // on '\n' (see ugfx_ui_draw_channel_message) so the filename is visible
    // to the user without needing a long single-line message.
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "%s\nFailed to load: %s",
             basename_of(filepath), esp_err_to_name(error));
    p3a_render_set_channel_message("Playback Error", P3A_CHANNEL_MSG_ERROR, -1, error_msg);
}

// Clear in-flight swap state and back buffer. Returns true if a swap was
// actually pending (matches old had_swap_request semantics).
static bool clear_pending_swap_state(void)
{
    bool had_swap_request = false;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        had_swap_request = s_swap_requested;
        s_swap_requested = false;
        s_loader_busy = false;
        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }
        xSemaphoreGive(s_buffer_mutex);
    }
    return had_swap_request;
}

// SWAP_FAIL_SILENT path: silently retry by emitting a fresh SWAP_NEXT event so
// the scheduler picks another artwork. Falls back to displaying an error after
// MAX_AUTO_RETRIES consecutive failures.
static void discard_failed_silent_swap(esp_err_t error, int32_t post_id, const char *filepath)
{
    bool had_swap_request = clear_pending_swap_state();
    if (!had_swap_request) {
        return;
    }

    auto_retry_blocklist_add(post_id);
    s_auto_retry_count++;
    s_auto_retry_skip_count = 0;

    if (s_auto_retry_count < MAX_AUTO_RETRIES) {
        ESP_LOGW(TAG, "Silent swap load failed (%s, post_id=%ld, file=%s); retrying (%d/%d)",
                 esp_err_to_name(error), (long)post_id, basename_of(filepath),
                 s_auto_retry_count, MAX_AUTO_RETRIES);
        event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
    } else {
        ESP_LOGE(TAG, "Silent swap load failed (%s, file=%s) %d times; giving up and displaying error",
                 esp_err_to_name(error), basename_of(filepath), s_auto_retry_count);
        show_load_error_message(error, filepath);
        animation_loader_reset_auto_retry_state();
        // Terminal failure for a user-initiated swap (no-op for auto-swap).
        if (proc_notif_fail_if_processing) proc_notif_fail_if_processing();
    }
}

// SWAP_FAIL_LOUD path: surface the failure on screen (no silent retry).
static void discard_failed_loud_swap(esp_err_t error, const char *filepath)
{
    bool had_swap_request = clear_pending_swap_state();
    if (!had_swap_request) {
        return;
    }

    ESP_LOGW(TAG, "Loud swap failed (%s, file=%s); displaying error.",
             esp_err_to_name(error), basename_of(filepath));
    animation_loader_reset_auto_retry_state();
    show_load_error_message(error, filepath);
    // Terminal failure for a user-initiated swap (no-op for auto-swap).
    if (proc_notif_fail_if_processing) proc_notif_fail_if_processing();
}

static void discard_failed_swap_request(esp_err_t error, swap_fail_mode_t fail_mode,
                                        int32_t post_id, const char *filepath)
{
    switch (fail_mode) {
        case SWAP_FAIL_LOUD:
            discard_failed_loud_swap(error, filepath);
            break;
        case SWAP_FAIL_SILENT:
        default:
            discard_failed_silent_swap(error, post_id, filepath);
            break;
    }
}

// Clear an in-flight swap request without treating it as a failure (no auto-retry).
// Used for cases where a swap is intentionally ignored.
static void discard_ignored_swap_request(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = false;
        s_loader_busy = false;

        // Back buffer should not be populated for ignored swaps, but keep it safe.
        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }

        xSemaphoreGive(s_buffer_mutex);
    }
    // Same-file dedup is a no-op completion, not an error. Treat it as
    // success so the proc-notif triangle clears immediately for a
    // user-initiated swap (no-op for auto-swap, since success only acts
    // on non-IDLE state).
    if (proc_notif_success) proc_notif_success();
}

// Evict a post_id from LAi across all active channels.
// Uses O(1) hash lookups per channel, so misses are cheap.
static void animation_loader_evict_from_lai(int32_t post_id)
{
    if (post_id == 0) {
        return;
    }

    ps_state_t *state = ps_get_state();
    if (!state || !state->mutex) {
        return;
    }

    xSemaphoreTake(state->mutex, portMAX_DELAY);
    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        channel_cache_t *cache = ch->cache;
        if (!cache) {
            continue;
        }
        // lai_remove_entry takes its own mutex
        int removed_pos = -1;
        if (lai_remove_entry(cache, post_id, &removed_pos)) {
            ESP_LOGW(TAG, "Evicted corrupt entry from LAi: post_id=%ld, channel=%zu",
                     (long)post_id, i);
            if (removed_pos >= 0 && ch->cursor > (uint32_t)removed_pos) {
                ch->cursor--;
            }
            channel_cache_schedule_save(cache);
        }
    }
    xSemaphoreGive(state->mutex);

    download_manager_rescan();
}

// Exposed for prefetch-time corruption handling.
// Handles vault (/vault/), giphy (/giphy/), and museum (/museum/) cached files.
bool animation_loader_try_delete_corrupt_cached_file(const char *filepath, esp_err_t error, int32_t post_id)
{
    if (!filepath || (strstr(filepath, "/vault/") == NULL
                      && strstr(filepath, "/giphy/") == NULL
                      && strstr(filepath, "/museum/") == NULL)) {
        return false;
    }

    // SAFEGUARD: Only delete if first time since boot OR cooldown has elapsed
    uint64_t current_time_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool can_delete = false;

    if (s_last_corrupt_deletion_ms == 0) {
        can_delete = true;
    } else {
        uint64_t time_since_last;
        if (current_time_ms >= s_last_corrupt_deletion_ms) {
            time_since_last = current_time_ms - s_last_corrupt_deletion_ms;
        } else {
            time_since_last = CORRUPT_DELETION_COOLDOWN_MS;
        }
        if (time_since_last >= CORRUPT_DELETION_COOLDOWN_MS) {
            can_delete = true;
        }
    }

    if (!can_delete) {
        return false;
    }

    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "DELETING CORRUPT CACHED FILE");
    ESP_LOGE(TAG, "File: %s", filepath);
    ESP_LOGE(TAG, "Error: %s", esp_err_to_name(error));
    ESP_LOGE(TAG, "Reason: File failed to decode/prefetch, marking as corrupt");
    ESP_LOGE(TAG, "Action: Deleting file so it can be re-downloaded");
    ESP_LOGE(TAG, "========================================");

    if (unlink(filepath) == 0) {
        s_last_corrupt_deletion_ms = current_time_ms;
        ESP_LOGI(TAG, "Successfully deleted corrupt file. Will be re-downloaded on next channel refresh.");

        // Remove from LAi so the scheduler doesn't re-pick this entry
        animation_loader_evict_from_lai(post_id);

        return true;
    }

    ESP_LOGW(TAG, "Failed to delete corrupt file: %s (errno=%d)", filepath, errno);
    return false;
}

void animation_loader_wait_for_idle(void)
{
    while (true) {
        bool busy = false;
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            busy = s_loader_busy;
            xSemaphoreGive(s_buffer_mutex);
        }
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void animation_loader_task(void *arg)
{
    (void)arg;

    while (true) {
        // Use timeout so we can periodically check if front buffer needs loading
        // This handles the boot case where no animation was initially available
        bool semaphore_signaled = (xSemaphoreTake(s_loader_sem, pdMS_TO_TICKS(1000)) == pdTRUE);

        bool swap_was_requested = false;
        bool front_buffer_needs_loading = false;

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            swap_was_requested = s_swap_requested;
            front_buffer_needs_loading = !s_front_buffer.ready && !s_front_buffer.decoder;
            
            // If no signal and front buffer is ready, just continue waiting
            if (!semaphore_signaled && !front_buffer_needs_loading) {
                xSemaphoreGive(s_buffer_mutex);
                continue;
            }
            
            // NOTE: Boot loading was removed. The channel system now uses swap_to(0, 0)
            // to initiate playback through the standard swap mechanism. This ensures
            // the channel state is properly initialized and the auto-swap timer starts
            // correctly after the first real swap.
            
            // Skip loading if in UI mode and not triggered by exit_ui_mode
            if (display_renderer_is_ui_mode() && !swap_was_requested) {
                ESP_LOGD(TAG, "Loader task: Skipping load during UI mode");
                xSemaphoreGive(s_buffer_mutex);
                continue;
            }
            
            // CRITICAL: Wait for any in-progress prefetch to complete before loading.
            // The render task may be using the back buffer's decoder and frame buffers
            // for prefetch (outside the mutex). Starting a new load would call
            // unload_animation_buffer() which frees memory the render task is using,
            // causing heap corruption (use-after-free → crash in tlsf_free).
            //
            // Check BOTH flags:
            // - prefetch_pending: prefetch has been requested (loader set it)
            // - prefetch_in_progress: prefetch is actively executing (render task set it)
            if (s_back_buffer.prefetch_pending || s_back_buffer.prefetch_in_progress) {
                ESP_LOGD(TAG, "Loader task: waiting for prefetch to complete...");
                xSemaphoreGive(s_buffer_mutex);
                // Wait for render task to signal prefetch completion (deadlock breaker;
                // a single first-frame decode normally completes well under a second).
                if (xSemaphoreTake(s_prefetch_done_sem, pdMS_TO_TICKS(15000)) == pdFALSE) {
                    ESP_LOGW(TAG, "Loader task: prefetch wait timed out after 15s");
                }
                // Re-queue ourselves to check if prefetch is now done
                if (s_loader_sem) {
                    xSemaphoreGive(s_loader_sem);
                }
                continue;
            }
            
            s_loader_busy = true;
            xSemaphoreGive(s_buffer_mutex);
        } else {
            continue;
        }

        // If animation_player_request_swap() provided an override, use it for this load.
        animation_load_override_t ov = {0};
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            ov = s_load_override;
            if (s_load_override.valid) {
                // Consume one-shot override
                s_load_override.valid = false;
            }
            xSemaphoreGive(s_buffer_mutex);
        }

        const char *filepath = NULL;
        asset_type_t type = ASSET_TYPE_WEBP;
        ps_channel_type_t channel_type = PS_CHANNEL_TYPE_NAMED;  // default
        const char *name_for_log = NULL;
        int32_t post_id = 0;
        const char *channel_spec_name = "";
        const char *channel_identifier = "";
        swap_fail_mode_t fail_mode = SWAP_FAIL_SILENT;

        post_source_t post_source = POST_SOURCE_NONE;

        if (ov.valid) {
            filepath = ov.filepath;
            type = ov.type;
            channel_type = ov.channel_type;
            channel_spec_name = ov.channel_spec_name;
            channel_identifier = ov.channel_identifier;
            name_for_log = ov.filepath;
            post_id = ov.post_id;
            post_source = ov.post_source;
            fail_mode = ov.fail_mode;
            ESP_LOGD(TAG, "Loader task: swap request: %s (type=%d post_id=%d fail_mode=%d)",
                     filepath, (int)type, (int)post_id, (int)fail_mode);
        } else if (swap_was_requested) {
            // Get current artwork from play_scheduler only if an actual swap was requested
            queued_item_t current = {0};
            esp_err_t err = playback_queue_current(&current);
            if (err != ESP_OK || current.request.filepath[0] == '\0') {
                // No current artwork. This is expected after exiting UI mode
                // (provisioning/OTA) where metadata was cleared. Silently clear
                // the swap request instead of showing a "Playback Error" message;
                // the provisioning exit path triggers play_scheduler_next() to
                // pick a new animation.
                ESP_LOGD(TAG, "Loader task: No current artwork (likely post-UI-mode exit)");
                if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                    s_swap_requested = false;
                    s_loader_busy = false;
                    xSemaphoreGive(s_buffer_mutex);
                }
                continue;
            }
            // Use static buffers to hold strings (play_scheduler_current returns by value)
            static char s_current_filepath[256];
            static char s_current_channel_spec_name[33];
            static char s_current_channel_identifier[33];
            strlcpy(s_current_filepath, current.request.filepath, sizeof(s_current_filepath));
            strlcpy(s_current_channel_spec_name, current.request.channel_spec_name,
                    sizeof(s_current_channel_spec_name));
            strlcpy(s_current_channel_identifier, current.request.channel_identifier,
                    sizeof(s_current_channel_identifier));
            filepath = s_current_filepath;
            type = current.request.type;
            channel_type = current.request.channel_type;
            channel_spec_name = s_current_channel_spec_name;
            channel_identifier = s_current_channel_identifier;
            name_for_log = current.request.filepath;
            post_id = current.request.post_id;
            post_source = current.request.post_source;
        } else {
            // No swap request and no override - nothing to do
            // This handles the case where we woke up due to timeout while waiting
            // for the channel to become ready. Just mark as not busy and continue.
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                s_loader_busy = false;
                xSemaphoreGive(s_buffer_mutex);
            }
            continue;
        }

        // If the target filepath is exactly the same as what we're already playing,
        // ignore the swap entirely.
        if (!ov.valid && filepath) {
            bool same_as_current = false;
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                const char *current_fp = s_front_buffer.filepath;
                if (current_fp && strcmp(current_fp, filepath) == 0) {
                    same_as_current = true;
                }
                xSemaphoreGive(s_buffer_mutex);
            }

            if (same_as_current) {
                ESP_LOGI(TAG, "Loader task: Ignoring swap request (already playing): %s", filepath);
                discard_ignored_swap_request();
                continue;
            }
        }

        // Silent-retry blocklist: if the picker handed us a post_id we already
        // know is broken in this burst, skip the load entirely and request
        // another pick. Doesn't consume the retry budget but is bounded by
        // MAX_BLOCKLIST_SKIPS to prevent a stuck picker from looping forever.
        if (fail_mode == SWAP_FAIL_SILENT && auto_retry_is_blocklisted(post_id)) {
            s_auto_retry_skip_count++;
            ESP_LOGW(TAG, "Silent swap: skipping blocklisted post_id=%ld (%s) [skip %d/%d]",
                     (long)post_id, filepath ? filepath : "(null)",
                     s_auto_retry_skip_count, MAX_BLOCKLIST_SKIPS);
            (void)clear_pending_swap_state();
            if (s_auto_retry_skip_count >= MAX_BLOCKLIST_SKIPS) {
                ESP_LOGE(TAG, "Silent swap: too many blocklist skips, giving up and displaying error");
                show_load_error_message(ESP_ERR_NOT_FOUND, filepath);
                animation_loader_reset_auto_retry_state();
                // Terminal failure for a user-initiated swap (no-op for auto-swap).
                if (proc_notif_fail_if_processing) proc_notif_fail_if_processing();
            } else {
                event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
            }
            continue;
        }

        ESP_LOGD(TAG, "Loader task: Loading animation '%s' into back buffer", name_for_log ? name_for_log : "(null)");

        // Check if file exists BEFORE trying to load.
        // This distinguishes "missing" from "corrupt decode" (especially for vault).
        bool file_missing = false;
        if (filepath) {
            struct stat st;
            if (stat(filepath, &st) != 0) {
                file_missing = true;
                ESP_LOGW(TAG, "File missing: %s", filepath);
            }
        } else {
            file_missing = true;
        }

        esp_err_t err = file_missing ? ESP_ERR_NOT_FOUND : load_animation_into_buffer(filepath, type, channel_type,
                                                                                      &s_back_buffer);
        if (err != ESP_OK) {
            bool is_cached_file = filepath && (strstr(filepath, "/vault/") != NULL
                                               || strstr(filepath, "/giphy/") != NULL
                                               || strstr(filepath, "/museum/") != NULL);

            if (is_cached_file && file_missing) {
                // File doesn't exist - advance to next and let background refresh re-download it
                // Phase 3: No navigation on load failure
                ESP_LOGW(TAG, "Missing cached file: %s", filepath);
            } else if (file_missing) {
                // Phase 3: No navigation on load failure
                ESP_LOGW(TAG, "Missing file: %s", filepath ? filepath : "(null)");
            } else if (is_cached_file) {
                // File exists but failed to decode - it's corrupt
                (void)animation_loader_try_delete_corrupt_cached_file(filepath, err, post_id);
            } else {
                // Phase 3: No navigation on load failure
                ESP_LOGW(TAG, "Decode failed: %s", filepath ? filepath : "(null)");
            }

            // SWAP_FAIL_SILENT retries up to MAX_AUTO_RETRIES; SWAP_FAIL_LOUD
            // surfaces the error on screen immediately.
            discard_failed_swap_request(err, fail_mode, post_id, filepath);
            continue;
        }

        // Touch file mtime for LRU tracking AFTER successful load.
        // This ensures files that fail to load (corrupt, too large) are not
        // touched, making them natural candidates for storage eviction.
        if (filepath && sntp_sync_is_synchronized()) {
            time_t now = time(NULL);
            struct utimbuf times = { now, now };
            utime(filepath, &times);
        }

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            s_back_buffer.prefetch_pending = true;
            s_back_buffer.ready = false;
            s_back_buffer.post_id = post_id;  // For view tracking
            s_back_buffer.post_source = post_source;
            s_back_buffer.view_channel_type = channel_type;
            strlcpy(s_back_buffer.view_channel_spec_name, channel_spec_name,
                    sizeof(s_back_buffer.view_channel_spec_name));
            strlcpy(s_back_buffer.view_channel_identifier, channel_identifier,
                    sizeof(s_back_buffer.view_channel_identifier));
            if (swap_was_requested) {
                s_swap_requested = true;
                ESP_LOGD(TAG, "Loader task: Swap was requested, will swap after prefetch");
            }
            s_loader_busy = false;
            xSemaphoreGive(s_buffer_mutex);
        }

        ESP_LOGD(TAG, "Loader task: Successfully loaded animation '%s' (prefetch_pending=true)", name_for_log ? name_for_log : "(null)");
    }
}

void free_sd_file_list(void)
{
    // Legacy function - no longer needed with channel abstraction
    (void)s_sd_file_list;
}

bool directory_has_animation_files(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "directory_has_animation_files: Failed to open %s", dir_path);
        return false;
    }

    struct dirent *entry;
    bool has_anim = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            const char *name = entry->d_name;
            size_t len = strlen(name);
            // Check longer extensions first (e.g., .jpeg before .jpg), all comparisons are case-insensitive
            if ((len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".apng") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0)) {
                has_anim = true;
                break;
            }
        }
    }
    closedir(dir);

    return has_anim;
}

esp_err_t find_animations_directory(const char *root_path, char **found_dir_out)
{
    ESP_LOGI(TAG, "Searching in: %s", root_path);

    DIR *dir = opendir(root_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", root_path, errno);
        return ESP_FAIL;
    }

    if (directory_has_animation_files(root_path)) {
        size_t len = strlen(root_path);
        *found_dir_out = (char *)malloc(len + 1);
        if (!*found_dir_out) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        strcpy(*found_dir_out, root_path);
        closedir(dir);
        ESP_LOGI(TAG, "Found animations directory: %s", *found_dir_out);
        return ESP_OK;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char subdir_path[512];
        int ret = snprintf(subdir_path, sizeof(subdir_path), "%s/%s", root_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(subdir_path)) {
            continue;
        }

        struct stat st;
        if (stat(subdir_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = find_animations_directory(subdir_path, found_dir_out);
            if (err == ESP_OK) {
                closedir(dir);
                return ESP_OK;
            }
        }
    }

    closedir(dir);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t refresh_animation_file_list(void)
{
    if (!s_sd_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    return play_scheduler_refresh_sdcard_cache();
}

void unload_animation_buffer(animation_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    loaded_animation_t loaded = {
        .decoder = buf->decoder,
        .file_data = (uint8_t *)buf->file_data,
        .file_size = buf->file_size,
        .info = buf->decoder_info,
    };
    loader_service_unload(&loaded);
    buf->decoder = NULL;
    buf->file_data = NULL;
    buf->file_size = 0;

    free(buf->native_frame_b1);
    free(buf->native_frame_b2);
    buf->native_frame_b1 = NULL;
    buf->native_frame_b2 = NULL;
    buf->native_buffer_active = 0;
    buf->native_frame_size = 0;

    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);
    buf->upscale_lookup_x = NULL;
    buf->upscale_lookup_y = NULL;
    buf->upscale_src_w = 0;
    buf->upscale_src_h = 0;
    buf->upscale_dst_w = 0;
    buf->upscale_dst_h = 0;
    buf->upscale_offset_x = 0;
    buf->upscale_offset_y = 0;
    buf->upscale_scaled_w = 0;
    buf->upscale_scaled_h = 0;
    buf->upscale_has_borders = false;
    buf->upscale_rotation_built = DISPLAY_ROTATION_0;

    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;
    buf->prefetch_in_progress = false;
    buf->prefetched_first_frame_delay_ms = 1;
    buf->current_frame_delay_ms = 1;
    buf->static_frame_cached = false;
    buf->static_bg_generation = 0;

    free(buf->filepath);
    buf->filepath = NULL;

    buf->ready = false;
    memset(&buf->decoder_info, 0, sizeof(buf->decoder_info));
    buf->asset_index = 0;
}

// ============================================================================
// Upscale map building (aspect-ratio preserving + rotation-aware)
// ============================================================================

static esp_err_t build_upscale_maps_for_buffer(animation_buffer_t *buf, int canvas_w, int canvas_h, display_rotation_t rotation)
{
    if (!buf || canvas_w <= 0 || canvas_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int target_w = EXAMPLE_LCD_H_RES;
    const int target_h = EXAMPLE_LCD_V_RES;

    // Compute scaled rectangle in PHYSICAL framebuffer coordinates (dst_x/dst_y).
    // For 90/270 we swap source dimensions for aspect-ratio decisions (matches visual rotation),
    // but lookup tables always map to the original source axes:
    // - lookup_x maps to source X in [0, canvas_w)
    // - lookup_y maps to source Y in [0, canvas_h)
    const bool swap_src = (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270);
    const int src_w_eff = swap_src ? canvas_h : canvas_w;
    const int src_h_eff = swap_src ? canvas_w : canvas_h;

    int scaled_w = target_w;
    int scaled_h = target_h;

    // Fit-to-screen, preserve aspect ratio (no cropping).
    if ((int64_t)src_w_eff * (int64_t)target_h >= (int64_t)src_h_eff * (int64_t)target_w) {
        // Source wider (or equal): fit width
        scaled_w = target_w;
        scaled_h = (int)(((int64_t)target_w * (int64_t)src_h_eff) / (int64_t)src_w_eff);
    } else {
        // Source taller: fit height
        scaled_h = target_h;
        scaled_w = (int)(((int64_t)target_h * (int64_t)src_w_eff) / (int64_t)src_h_eff);
    }

    if (scaled_w < 1) scaled_w = 1;
    if (scaled_h < 1) scaled_h = 1;
    if (scaled_w > target_w) scaled_w = target_w;
    if (scaled_h > target_h) scaled_h = target_h;

    const int offset_x = (target_w - scaled_w) / 2;
    const int offset_y = (target_h - scaled_h) / 2;
    const bool has_borders = (offset_x > 0) || (offset_y > 0);

    // Lookup lengths depend on rotation because the blitter indexes lookup_x by dst_x for 0/180
    // but by dst_y for 90/270 (and lookup_y vice-versa).
    //
    // IMPORTANT: We do NOT want to free/allocate lookup tables repeatedly under heavy swap/rotate spam.
    // That creates heap churn and amplifies the impact of any latent corruption. Instead we allocate
    // both tables once at a fixed "max" length and only rewrite the active prefix.
    const int used_lookup_x_len = swap_src ? scaled_h : scaled_w; // maps to source X (canvas_w)
    const int used_lookup_y_len = swap_src ? scaled_w : scaled_h; // maps to source Y (canvas_h)
    const int max_len = (target_w > target_h) ? target_w : target_h;

    if (!buf->upscale_lookup_x) {
        buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc((size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!buf->upscale_lookup_x) {
            ESP_LOGE(TAG, "Failed to allocate upscale lookup X (max_len=%d)", max_len);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!buf->upscale_lookup_y) {
        buf->upscale_lookup_y = (uint16_t *)heap_caps_malloc((size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!buf->upscale_lookup_y) {
            ESP_LOGE(TAG, "Failed to allocate upscale lookup Y (max_len=%d)", max_len);
            heap_caps_free(buf->upscale_lookup_x);
            buf->upscale_lookup_x = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    // Build lookup_x -> source X in [0, canvas_w) for the active prefix
    for (int i = 0; i < used_lookup_x_len; ++i) {
        int src_x = (i * canvas_w) / used_lookup_x_len;
        if (src_x >= canvas_w) src_x = canvas_w - 1;
        if (src_x < 0) src_x = 0;
        buf->upscale_lookup_x[i] = (uint16_t)src_x;
    }
    // Fill remainder defensively with last valid value
    if (used_lookup_x_len > 0) {
        const uint16_t last = buf->upscale_lookup_x[used_lookup_x_len - 1];
        for (int i = used_lookup_x_len; i < max_len; ++i) {
            buf->upscale_lookup_x[i] = last;
        }
    }

    // Build lookup_y -> source Y in [0, canvas_h) for the active prefix
    for (int i = 0; i < used_lookup_y_len; ++i) {
        int src_y = (i * canvas_h) / used_lookup_y_len;
        if (src_y >= canvas_h) src_y = canvas_h - 1;
        if (src_y < 0) src_y = 0;
        buf->upscale_lookup_y[i] = (uint16_t)src_y;
    }
    if (used_lookup_y_len > 0) {
        const uint16_t last = buf->upscale_lookup_y[used_lookup_y_len - 1];
        for (int i = used_lookup_y_len; i < max_len; ++i) {
            buf->upscale_lookup_y[i] = last;
        }
    }

    buf->upscale_src_w = canvas_w;
    buf->upscale_src_h = canvas_h;
    buf->upscale_dst_w = target_w;
    buf->upscale_dst_h = target_h;
    buf->upscale_offset_x = offset_x;
    buf->upscale_offset_y = offset_y;
    buf->upscale_scaled_w = scaled_w;
    buf->upscale_scaled_h = scaled_h;
    buf->upscale_has_borders = has_borders;
    buf->upscale_rotation_built = rotation;

    ESP_LOGD(TAG, "Upscale maps: %dx%d -> %dx%d (offset %d,%d, scaled %dx%d, borders=%d, rot=%d)",
             canvas_w, canvas_h, target_w, target_h, offset_x, offset_y, scaled_w, scaled_h,
             (int)has_borders, (int)rotation);

    return ESP_OK;
}

esp_err_t animation_loader_rebuild_upscale_maps(animation_buffer_t *buf, display_rotation_t rotation)
{
    if (!buf || !buf->decoder) {
        return ESP_ERR_INVALID_STATE;
    }
    const int canvas_w = (int)buf->decoder_info.canvas_width;
    const int canvas_h = (int)buf->decoder_info.canvas_height;
    if (canvas_w <= 0 || canvas_h <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return build_upscale_maps_for_buffer(buf, canvas_w, canvas_h, rotation);
}

static esp_err_t init_animation_decoder_for_buffer(animation_buffer_t *buf,
                                                   animation_decoder_t *decoder,
                                                   const animation_decoder_info_t *info)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!decoder || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    buf->decoder = decoder;
    buf->decoder_info = *info;

    // One-time diagnostic (DEBUG): how this asset flows through the pipeline.
    {
        uint8_t br = 0, bg = 0, bb = 0;
        config_store_get_background_color(&br, &bg, &bb);
        ESP_LOGD(TAG, "Decoder: %ux%u frames=%u transp=%d bg=(%u,%u,%u)",
                 (unsigned)buf->decoder_info.canvas_width,
                 (unsigned)buf->decoder_info.canvas_height,
                 (unsigned)buf->decoder_info.frame_count,
                 (int)buf->decoder_info.has_transparency,
                 (unsigned)br, (unsigned)bg, (unsigned)bb);
    }

    const int canvas_w = (int)buf->decoder_info.canvas_width;
    const int canvas_h = (int)buf->decoder_info.canvas_height;
    // All decoders output RGB888 (3 bytes per pixel), with alpha pre-composited against background
    buf->native_frame_size = (size_t)canvas_w * canvas_h * 3;

    // Allocate native frame buffers, preferring PSRAM for large buffers
    buf->native_frame_b1 = (uint8_t *)heap_caps_malloc(buf->native_frame_size,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf->native_frame_b1) {
        buf->native_frame_b1 = (uint8_t *)malloc(buf->native_frame_size);
    }
    if (!buf->native_frame_b1) {
        ESP_LOGE(TAG, "Failed to allocate native frame buffer B1 (%zu bytes)", buf->native_frame_size);
        // Decoder ownership stays with the caller's `loaded` — see map_err path below.
        buf->decoder = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate B2 only for animated formats. The static-image fast path in
    // animation_player_render.c renders directly from B1 every tick (no decode-
    // ahead ping-pong), so B2 would just sit unused. Skipping it for
    // frame_count <= 1 reclaims one full canvas-sized buffer per static asset
    // (e.g. ~4 MiB for an oversized JPEG decoded at 3/8 to 1351x1013), which
    // is the difference between fitting two consecutive large statics in PSRAM
    // and tripping the silent-retry path on swap.
    if (buf->decoder_info.frame_count > 1) {
        buf->native_frame_b2 = (uint8_t *)heap_caps_malloc(buf->native_frame_size,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf->native_frame_b2) {
            buf->native_frame_b2 = (uint8_t *)malloc(buf->native_frame_size);
        }
        if (!buf->native_frame_b2) {
            ESP_LOGE(TAG, "Failed to allocate native frame buffer B2 (%zu bytes)", buf->native_frame_size);
            free(buf->native_frame_b1);
            buf->native_frame_b1 = NULL;
            buf->decoder = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    buf->native_buffer_active = 0;

    // Build aspect-ratio preserving lookup maps for the CURRENT rotation.
    // If rotation changes later, the maps must be rebuilt.
    esp_err_t map_err = build_upscale_maps_for_buffer(buf, canvas_w, canvas_h, display_renderer_get_rotation());
    if (map_err != ESP_OK) {
        // Clean up resources allocated by THIS function only.
        // Don't free decoder or file_data - caller will do that via loader_service_unload()
        free(buf->native_frame_b1);
        free(buf->native_frame_b2);
        buf->native_frame_b1 = NULL;
        buf->native_frame_b2 = NULL;
        heap_caps_free(buf->upscale_lookup_x);
        heap_caps_free(buf->upscale_lookup_y);
        buf->upscale_lookup_x = NULL;
        buf->upscale_lookup_y = NULL;
        // Clear our references but don't free - ownership returns to caller
        buf->decoder = NULL;
        buf->file_data = NULL;
        return map_err;
    }

    return ESP_OK;
}

esp_err_t load_animation_into_buffer(const char *filepath, asset_type_t type, ps_channel_type_t channel_type,
                                     animation_buffer_t *buf)
{
    if (!buf || !filepath) {
        return ESP_ERR_INVALID_ARG;
    }

    unload_animation_buffer(buf);

    animation_decoder_type_t decoder_type;
    if (type == ASSET_TYPE_WEBP) {
        decoder_type = ANIMATION_DECODER_TYPE_WEBP;
    } else if (type == ASSET_TYPE_GIF) {
        decoder_type = ANIMATION_DECODER_TYPE_GIF;
    } else if (type == ASSET_TYPE_PNG) {
        decoder_type = ANIMATION_DECODER_TYPE_PNG;
    } else if (type == ASSET_TYPE_JPEG) {
        decoder_type = ANIMATION_DECODER_TYPE_JPEG;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    loaded_animation_t loaded = {0};
    esp_err_t err = loader_service_load(filepath, decoder_type, &loaded);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load file: %s", esp_err_to_name(err));
        return err;
    }

    buf->file_data = loaded.file_data;
    buf->file_size = loaded.file_size;
    buf->type = type;
    buf->channel_type = channel_type;
    buf->asset_index = 0;  // Position tracking now managed by play_scheduler

    // Store filepath
    buf->filepath = strdup(filepath);
    if (!buf->filepath) {
        ESP_LOGE(TAG, "Failed to duplicate filepath");
        loader_service_unload(&loaded);
        buf->file_data = NULL;
        buf->file_size = 0;
        return ESP_ERR_NO_MEM;
    }

    err = init_animation_decoder_for_buffer(buf, loaded.decoder, &loaded.info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize animation decoder '%s': %s", filepath, esp_err_to_name(err));
        loader_service_unload(&loaded);
        buf->file_data = NULL;
        buf->file_size = 0;
        free(buf->filepath);
        buf->filepath = NULL;
        return err;
    }

    loaded.decoder = NULL;
    loaded.file_data = NULL;

    // No separate prefetch buffer needed - the first frame is decoded to native_frame_b1
    // during prefetch, then upscaled directly to the display back buffer when displayed
    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;
    buf->prefetch_in_progress = false;

    ESP_LOGD(TAG, "Loaded: %s", filepath);

    return ESP_OK;
}

size_t get_next_asset_index(size_t current_index)
{
    (void)current_index;
    return 0;
}

size_t get_previous_asset_index(size_t current_index)
{
    (void)current_index;
    return 0;
}

esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index)
{
    (void)filename;
    (void)animations_dir;
    (void)insert_after_index;
    (void)out_index;
    
    ESP_LOGW(TAG, "animation_player_add_file: Not supported with channel abstraction.");
    return ESP_ERR_NOT_SUPPORTED;
}
