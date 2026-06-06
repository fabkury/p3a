// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file download_manager.c
 * @brief Decoupled download manager with own state
 *
 * Downloads files one at a time using round-robin across channels.
 * Owns its own channel list and download cursors - fully decoupled from
 * Play Scheduler.
 *
 * Architecture:
 * - Receives channel list via download_manager_set_channels()
 * - Round-robin across channels to find next missing file
 * - Uses channel_cache APIs to find entries needing download
 * - Sleeps when nothing to download
 */

#include "download_manager.h"
#include "play_scheduler.h"
#include "storage_eviction.h"
#include "sntp_sync.h"
#include "esp_heap_caps.h"
#include "makapix_channel_impl.h"
#include "makapix_channel_utils.h"
#include "makapix_artwork.h"
#include "makapix_channel_events.h"
#include "giphy.h"
#include "art_institution.h"
#include "channel_cache.h"
#include "lai_verify.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "p3a_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "dl_mgr";

// External: check if SD is paused for OTA
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

// External: animation player and render functions
extern bool animation_player_is_animation_ready(void);
extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);

// ============================================================================
// Helpers - Display Name (delegates to play_scheduler's canonical implementation)
// ============================================================================

static void dl_get_display_name(const char *channel_id, char *out_name, size_t max_len)
{
    ps_get_display_name(channel_id, out_name, max_len);
}

// ============================================================================
// Download Manager State (decoupled from Play Scheduler)
// ============================================================================

#define DL_MAX_CHANNELS PS_MAX_CHANNELS

typedef struct {
    char channel_id[64];
    uint32_t dl_cursor;          // Current position for scanning
    uint32_t scan_epoch_start;   // Where this scan pass began
    bool has_wrapped;            // Whether cursor has wrapped from end to 0
    bool channel_complete;       // All entries scanned this epoch
} dl_channel_state_t;

/**
 * @brief Snapshot of channel state for thread-safe operation
 *
 * Used by dl_get_next_download() to operate on a consistent copy of
 * channel state without holding the mutex during file operations.
 */
typedef struct {
    dl_channel_state_t channels[DL_MAX_CHANNELS];
    size_t channel_count;
    size_t round_robin_idx;
    uint32_t epoch;
} dl_snapshot_t;

static dl_channel_state_t s_dl_channels[DL_MAX_CHANNELS];
static size_t s_dl_channel_count = 0;
static size_t s_dl_round_robin_idx = 0;
static uint32_t s_dl_epoch = 0;  // Incremented on channel/cursor changes; guards stale commits

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// Current download state
static char s_active_channel[64] = {0};
static bool s_busy = false;
static bool s_all_downloaded_logged = false;  // Suppress repeated "All files downloaded" logs

// S2: the download manager used to maintain a parallel "playback_initiated"
// flag that gated its own play_scheduler_next() trigger when the first file
// landed. That created a race with the LAi 0→1 trigger in
// play_scheduler_lai.c (both fired on the same event from different tasks,
// with separate flags that didn't see each other — see the analysis in
// docs/observability/multi-swap-race.md). The flag and both Path B triggers
// were removed. Initial playback is now driven exclusively by
// play_scheduler_on_download_complete → ps_lai_add zero-to-one check.

// Cooperative cancellation flag (S1). Set by download_manager_set_channels
// when the in-flight download's channel is no longer in the active set;
// polled by each chunked downloader between read iterations; cleared at
// the start of each new download attempt in the download task. Single-byte
// writes are atomic on ESP32, so volatile is sufficient — no mutex needed
// on the read path (the downloader is the only reader and the only place
// that observes a stale "true" would just bail out one chunk later, which
// is harmless).
static volatile bool s_dl_cancel_requested = false;

// PSRAM-backed static task stack for reduced internal RAM usage
static StackType_t *s_download_stack = NULL;
static StaticTask_t s_download_task_buffer;
static bool s_download_stack_allocated = false;

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static void set_busy(bool busy, const char *channel_id)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_busy = busy;
        if (busy && channel_id && channel_id[0]) {
            strlcpy(s_active_channel, channel_id, sizeof(s_active_channel));
        } else if (!busy) {
            s_active_channel[0] = '\0';
        }
        xSemaphoreGive(s_mutex);
    } else {
        s_busy = busy;
    }
}

// Static buffer for 404 marker check (single-threaded access from download task)
static char s_marker_path[264];

static bool has_404_marker(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') return false;
    int ret = snprintf(s_marker_path, sizeof(s_marker_path), "%s.404", filepath);
    if (ret < 0 || ret >= (int)sizeof(s_marker_path)) return false;
    struct stat st;
    return (stat(s_marker_path, &st) == 0);
}

// Static buffers for dl_build_vault_filepath to reduce stack usage
// Safe because only one download task exists (single-threaded access)
static char s_build_vault_base[128];
static char s_build_storage_key[40];

/**
 * @brief Build vault filepath for an entry
 *
 * Uses hash sharding: {vault}/{d0}/{d1}/{storage_key}.{ext}
 * (see sd_path_build_sharded())
 */
static void dl_build_vault_filepath(const makapix_channel_entry_t *entry,
                                    char *out, size_t out_len)
{
    if (!entry || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    // Get vault base path
    esp_err_t path_err = sd_path_get_vault(s_build_vault_base, sizeof(s_build_vault_base));
    if (path_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot resolve vault directory: %s", esp_err_to_name(path_err));
        out[0] = '\0';
        return;
    }

    // Convert UUID bytes to string
    bytes_to_uuid(entry->storage_key_uuid, s_build_storage_key, sizeof(s_build_storage_key));

    if (makapix_build_vault_path(s_build_vault_base, s_build_storage_key, entry->extension,
                                 out, out_len) != ESP_OK && out && out_len > 0) {
        out[0] = '\0';
    }
}

esp_err_t download_manager_build_entry_filepath(const char *channel_id,
                                                const makapix_channel_entry_t *entry,
                                                char *out, size_t out_len)
{
    if (!channel_id || !entry || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    bool is_giphy = play_scheduler_is_giphy_channel(channel_id);
    bool is_institution = !is_giphy && play_scheduler_is_institution_channel(channel_id);

    if (is_giphy) {
        // Giphy channel: entry is giphy_channel_entry_t (same 64-byte slot)
        const giphy_channel_entry_t *ge = (const giphy_channel_entry_t *)entry;
        giphy_build_filepath(ge->giphy_id, ge->extension, out, out_len);
    } else if (is_institution) {
        // Institution channel: entry is institution_channel_entry_t.
        const institution_channel_entry_t *ie = (const institution_channel_entry_t *)entry;
        // Sentinel extensions (0xFF unresolved, 0xFE tombstone) never have
        // a downloadable file (see docs/art-institutions/finalized-design.md).
        if (ie->extension == 0xFF || ie->extension == 0xFE) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        // The {museum}/{shard}/... path needs the playset spec_name.
        char spec_name[33] = {0};
        if (play_scheduler_get_channel_spec_name(channel_id, spec_name,
                                                 sizeof(spec_name)) != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
        art_institution_build_vault_path_from_spec(spec_name, ie, out, out_len);
    } else {
        // Makapix channel: standard sharded vault filepath
        dl_build_vault_filepath(entry, out, out_len);
    }

    return (out[0] != '\0') ? ESP_OK : ESP_FAIL;
}

// ============================================================================
// Snapshot Functions for Thread-Safe Channel Access
// ============================================================================

/**
 * @brief Take snapshot of channel state under mutex
 *
 * Creates a local copy of channel state to avoid holding mutex during
 * file operations. This prevents race conditions with download_manager_set_channels().
 *
 * @param out_snapshot Output snapshot structure
 * @return true if snapshot was taken successfully, false if no channels or error
 */
static bool dl_take_snapshot(dl_snapshot_t *out_snapshot)
{
    if (!out_snapshot) return false;

    bool success = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_dl_channel_count > 0) {
            out_snapshot->channel_count = s_dl_channel_count;
            out_snapshot->round_robin_idx = s_dl_round_robin_idx;
            out_snapshot->epoch = s_dl_epoch;
            memcpy(out_snapshot->channels, s_dl_channels,
                   s_dl_channel_count * sizeof(dl_channel_state_t));
            success = true;
        } else {
            ESP_LOGD(TAG, "dl_take_snapshot: no channels configured");
        }
        xSemaphoreGive(s_mutex);
    } else {
        ESP_LOGW(TAG, "dl_take_snapshot: mutex take failed (s_mutex=%p)", s_mutex);
    }

    return success;
}

/**
 * @brief Commit round-robin index and cursor changes back to shared state
 *
 * Only commits if the channel list hasn't changed since snapshot was taken.
 * If channels changed, the new channel list takes precedence and our changes
 * are discarded (the new channel switch reset everything anyway).
 *
 * @param snapshot The snapshot used for the current operation (with updated cursors)
 * @param new_round_robin_idx The new round-robin index to commit
 */
static void dl_commit_state(const dl_snapshot_t *snapshot, size_t new_round_robin_idx)
{
    if (!snapshot) return;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Only commit if epoch matches — any channel switch, cursor reset,
        // or rescan since our snapshot invalidates our state
        if (s_dl_epoch == snapshot->epoch) {
            s_dl_round_robin_idx = new_round_robin_idx;

            // Also update cursor positions for channels we scanned
            for (size_t i = 0; i < snapshot->channel_count; i++) {
                // Find matching channel by ID and update its state
                for (size_t j = 0; j < s_dl_channel_count; j++) {
                    if (strcmp(s_dl_channels[j].channel_id, snapshot->channels[i].channel_id) == 0) {
                        s_dl_channels[j].dl_cursor = snapshot->channels[i].dl_cursor;
                        s_dl_channels[j].channel_complete = snapshot->channels[i].channel_complete;
                        break;
                    }
                }
            }
        }
        xSemaphoreGive(s_mutex);
    }
}

// Static buffers for dl_get_next_download to reduce stack usage
// Safe because only one download task exists (single-threaded access)
static char s_dl_filepath[256];
static char s_dl_storage_key[40];

// Batch buffer for reducing mutex contention during download scan
// Fetches multiple missing entries at once instead of per-entry mutex
#define DL_BATCH_SIZE 32
static makapix_channel_entry_t s_batch_entries[DL_BATCH_SIZE];

/**
 * @brief Get next download using round-robin across channels
 *
 * Operates on a snapshot of channel state to avoid race conditions.
 * Uses channel_cache APIs to find entries not yet in LAi (Locally Available index).
 * Scans channels in round-robin order, finding the first entry that:
 * - Is an artwork (not playlist) and not in LAi
 * - Does not have a .404 marker
 *
 * @param out_request Output download request
 * @param snapshot Snapshot of channel state (will be modified with cursor updates)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if all files downloaded
 */
static esp_err_t dl_get_next_download(download_request_t *out_request, dl_snapshot_t *snapshot,
                                      size_t *out_unresolved_count)
{
    if (out_unresolved_count) *out_unresolved_count = 0;
    if (!snapshot || snapshot->channel_count == 0 || !out_request) {
        ESP_LOGD(TAG, "dl_get_next_download invalid args (snapshot=%p, count=%zu)",
                 snapshot, snapshot ? snapshot->channel_count : 0);
        return ESP_ERR_NOT_FOUND;
    }

    // Try each channel in round-robin order
    for (size_t attempt = 0; attempt < snapshot->channel_count; attempt++) {
        size_t ch_idx = (snapshot->round_robin_idx + attempt) % snapshot->channel_count;
        dl_channel_state_t *ch = &snapshot->channels[ch_idx];

        char _dn[64];
        dl_get_display_name(ch->channel_id, _dn, sizeof(_dn));

        // Skip channels that don't need downloads (e.g., SD card - local files)
        if (!play_scheduler_needs_download(ch->channel_id)) {
            ESP_LOGD(TAG, "Skipping local channel '%s'", _dn);
            continue;
        }

        // Skip completed channels
        if (ch->channel_complete) {
            ESP_LOGD(TAG, "Skipping completed channel '%s'", _dn);
            continue;
        }

        // Pin the cache lifecycle for the duration of this channel's scan.
        // Without this, play_scheduler_execute_playset can free the cache
        // between registry_find and any subsequent cache->... dereference,
        // turning the rest of this loop into a use-after-free.
        channel_cache_lifecycle_lock();

        // Get channel cache from registry
        channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
        if (!cache) {
            ESP_LOGD(TAG, "Cache not found for '%s'", _dn);
            channel_cache_lifecycle_unlock();
            continue;
        }
        ESP_LOGD(TAG, "Checking cache '%s': entry_count=%zu available=%zu cursor=%lu epoch_start=%lu wrapped=%d",
                 ch->channel_id, cache->entry_count, cache->available_count,
                 (unsigned long)ch->dl_cursor, (unsigned long)ch->scan_epoch_start, ch->has_wrapped);

        // Find entries needing download using batch API (reduces mutex contention)
        bool found = false;
        uint32_t start_cursor = ch->dl_cursor;
        int scan_count = 0;
        size_t batch_idx = 0;
        size_t batch_count = 0;

        // Calculate effective end for this scan based on wrap state
        // After wrapping, we only scan up to scan_epoch_start (where we began)
        uint32_t effective_end = cache->entry_count;
        if (ch->has_wrapped && ch->scan_epoch_start > 0) {
            effective_end = ch->scan_epoch_start;
        }

        while (!found) {
            // Fetch next batch if current exhausted
            if (batch_idx >= batch_count) {
                if (channel_cache_get_missing_batch(cache, &ch->dl_cursor, effective_end,
                        s_batch_entries, DL_BATCH_SIZE, &batch_count) != ESP_OK) {
                    // Batch exhausted - check if we need to wrap or are complete
                    if (!ch->has_wrapped && ch->dl_cursor >= cache->entry_count) {
                        // First time reaching end - wrap to beginning
                        ch->dl_cursor = 0;
                        ch->has_wrapped = true;
                        ESP_LOGD(TAG, "Channel '%s' wrapping cursor to 0 (epoch_start=%lu)",
                                 ch->channel_id, (unsigned long)ch->scan_epoch_start);
                        
                        if (ch->scan_epoch_start == 0) {
                            // Started at 0, scanned everything, done
                            break;
                        }
                        
                        // Update effective_end for wrapped scan
                        effective_end = ch->scan_epoch_start;
                        batch_idx = batch_count = 0;  // Reset batch state
                        continue;  // Continue scanning from 0
                    }
                    break;  // Full scan complete or error
                }
                batch_idx = 0;
                scan_count += batch_count;
            }

            // Process current batch entry (mutex NOT held during this processing)
            makapix_channel_entry_t *entry = &s_batch_entries[batch_idx++];

            // Build filepath and storage key based on channel type
            bool is_giphy = play_scheduler_is_giphy_channel(ch->channel_id);
            bool is_institution = !is_giphy && play_scheduler_is_institution_channel(ch->channel_id);

            // Institution channels need the playset spec_name to derive
            // {museum}/{shard}/... vault paths and IIIF URLs.
            char ai_spec_name[33] = {0};
            char ai_museum_id[16] = {0};
            char ai_axis_unused[32] = {0};
            if (is_institution) {
                if (play_scheduler_get_channel_spec_name(ch->channel_id,
                                                        ai_spec_name, sizeof(ai_spec_name)) != ESP_OK) {
                    continue;  // Channel state vanished mid-scan; try next batch slot.
                }
                art_institution_parse_spec(ai_spec_name,
                                           ai_museum_id, sizeof(ai_museum_id),
                                           ai_axis_unused, sizeof(ai_axis_unused));

                // M2 sentinel extensions never have a downloadable file — skip.
                // 0xFF (unresolved) entries are work-in-progress: the resolver
                // will eventually mutate them to a downloadable form, so we
                // count them so the "all done" log doesn't lie. 0xFE
                // (tombstone) entries are terminal failures and don't count.
                const institution_channel_entry_t *ie = (const institution_channel_entry_t *)entry;
                if (ie->extension == 0xFF || ie->extension == 0xFE) {
                    ESP_LOGD(TAG, "SKIP post_id=%d: institution sentinel ext=0x%02X",
                             ie->post_id, ie->extension);
                    if (ie->extension == 0xFF && out_unresolved_count) {
                        (*out_unresolved_count)++;
                    }
                    continue;
                }
            }

            // Filepath via the shared helper (also used by the LAi
            // verification sweep, so both resolve identical paths).
            if (download_manager_build_entry_filepath(ch->channel_id, entry,
                                                      s_dl_filepath, sizeof(s_dl_filepath)) != ESP_OK) {
                continue;  // Un-pathable entry; skip to next batch slot.
            }

            // Storage key per channel type
            if (is_giphy) {
                const giphy_channel_entry_t *ge = (const giphy_channel_entry_t *)entry;
                strlcpy(s_dl_storage_key, ge->giphy_id, sizeof(s_dl_storage_key));
            } else if (is_institution) {
                const institution_channel_entry_t *ie = (const institution_channel_entry_t *)entry;
                strlcpy(s_dl_storage_key, ie->iiif_key, sizeof(s_dl_storage_key));
            } else {
                bytes_to_uuid(entry->storage_key_uuid, s_dl_storage_key, sizeof(s_dl_storage_key));
            }

            // Skip if 404 marker exists
            if (has_404_marker(s_dl_filepath)) {
                ESP_LOGD(TAG, "SKIP post_id=%d: has 404 marker (key=%.8s...)", entry->post_id, s_dl_storage_key);
                continue;
            }

            // Found entry needing download
            memset(out_request, 0, sizeof(*out_request));
            strlcpy(out_request->storage_key, s_dl_storage_key, sizeof(out_request->storage_key));
            strlcpy(out_request->filepath, s_dl_filepath, sizeof(out_request->filepath));
            strlcpy(out_request->channel_id, ch->channel_id, sizeof(out_request->channel_id));
            out_request->post_id = entry->post_id;  // Capture post_id for O(1) LAi lookup

            if (is_giphy) {
                // Build Giphy download URL from entry (respects downsized_medium override)
                giphy_build_download_url_for_entry((const giphy_channel_entry_t *)entry,
                                                  out_request->art_url, sizeof(out_request->art_url));
            } else if (is_institution) {
                const institution_channel_entry_t *ie = (const institution_channel_entry_t *)entry;
                art_institution_build_iiif_url(ai_museum_id, ie, 720,
                                               out_request->art_url, sizeof(out_request->art_url));
            } else {
                // Build the Makapix artwork URL via the shared remote shard helper.
                char shard[3 * MAKAPIX_REMOTE_SHARD_DEPTH];
                if (makapix_build_remote_shard(s_dl_storage_key, shard, sizeof(shard)) == ESP_OK) {
                    int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
                    snprintf(out_request->art_url, sizeof(out_request->art_url),
                             "https://%s/api/vault/%s/%s%s",
                             CONFIG_MAKAPIX_CLUB_HOST,
                             shard, s_dl_storage_key, s_ext_strings[ext_idx]);
                }
            }

            // Set cursor to just after this entry's position, not the batch end.
            // This ensures we don't skip other entries in the batch if this file
            // already exists on disk (detected later in download_task).
            uint32_t entry_ci = ci_find_by_post_id(cache, entry->post_id);
            if (entry_ci != UINT32_MAX) {
                ch->dl_cursor = entry_ci + 1;
            }
            // If lookup fails (shouldn't happen), cursor stays at batch end position

            found = true;
        }

        if (found) {
            // Advance round-robin to next channel for fairness and commit to shared state
            size_t new_rr_idx = (ch_idx + 1) % snapshot->channel_count;
            ESP_LOGD(TAG, "Found download: ch=%s key=%s (ci=%lu, cursor now %lu)",
                     ch->channel_id, out_request->storage_key,
                     (unsigned long)(ch->dl_cursor > 0 ? ch->dl_cursor - 1 : 0),
                     (unsigned long)ch->dl_cursor);
            channel_cache_lifecycle_unlock();
            dl_commit_state(snapshot, new_rr_idx);
            return ESP_OK;
        }

        // This channel scan is complete - mark it so we skip it in future iterations
        ch->channel_complete = true;
        ESP_LOGD(TAG, "Channel '%s' download scan complete (scanned %d entries, cursor %lu -> %lu, epoch_start=%lu, wrapped=%d, entry_count=%zu)",
                 ch->channel_id, scan_count, (unsigned long)start_cursor, (unsigned long)ch->dl_cursor,
                 (unsigned long)ch->scan_epoch_start, ch->has_wrapped, cache->entry_count);
        channel_cache_lifecycle_unlock();
    }

    // All channels complete - commit the completion state
    dl_commit_state(snapshot, snapshot->round_robin_idx);
    return ESP_ERR_NOT_FOUND;
}

// Static buffers to reduce stack usage in download_task
// These are safe because only one download task instance exists
static download_request_t s_dl_req;
static dl_snapshot_t s_dl_snapshot;
static char s_task_temp_path[264];    // For temp file checks
static char s_task_out_path[256];     // For download output path
static char s_task_marker_path[264];  // For 404 marker creation
static char s_task_display_name[64];  // For UI display name

static void dl_progress_cb(size_t bytes_read, size_t content_length, void *ctx)
{
    // The progress callback is registered when a download starts during fresh
    // start (!animation_player_is_animation_ready()). Playback may begin
    // mid-download (LAi zero-to-one transition triggers a swap), so we
    // re-check dynamically here and suppress further progress updates once
    // an animation is on screen.
    if (animation_player_is_animation_ready()) return;

    const char *name = (const char *)ctx;
    int pct = (content_length > 0) ? (int)((bytes_read * 100) / content_length) : -1;
    p3a_render_set_channel_message(name, 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, pct,
                                    "Downloading artwork...");
}

static void download_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Download task started");

    int loop_count = 0;
    time_t last_channel_eviction = 0;
    const time_t channel_eviction_interval = 8 * 3600;

    while (true) {
        // Every 100 iterations, log stack high water mark
        if (++loop_count % 100 == 0) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            if (hwm < 4096) {  // Warn if less than 16KB free (4096 * 4 bytes on ESP32)
                ESP_LOGW(TAG, "Stack HWM low: %lu words free (~%lu bytes)",
                         (unsigned long)hwm, (unsigned long)(hwm * sizeof(StackType_t)));
            }
        }

        // Periodic channel eviction (every 8 hours, only with valid clock)
        if (sntp_sync_is_synchronized()) {
            time_t now_evict = time(NULL);
            if (last_channel_eviction == 0) {
                last_channel_eviction = now_evict;  // seed timer on first sync
            } else if ((now_evict - last_channel_eviction) >= channel_eviction_interval) {
                last_channel_eviction = now_evict;
                channel_eviction_check_and_run();
            }
        }

        // Skip download cycle if PICO-8 mode is active
        if (p3a_state_get() == P3A_STATE_PICO8_STREAMING) {
            ESP_LOGD(TAG, "PICO-8 mode active, skipping download cycle");
            // Clear event before waiting to prevent a tight loop when the event
            // was already signaled (e.g. by download_manager_wake during exit).
            makapix_channel_clear_downloads_needed();
            makapix_channel_wait_for_downloads_needed(pdMS_TO_TICKS(5000));
            continue;
        }

        // Wait for prerequisites - WiFi and SD card must be available
        // NOTE: We no longer wait for full refresh completion. The refresh task
        // signals downloads_needed after each batch, allowing early downloads
        // as soon as the first batch of index entries arrives.
        if (!makapix_channel_is_wifi_ready()) {
            ESP_LOGI(TAG, "Waiting for WiFi...");
            makapix_channel_wait_for_wifi(portMAX_DELAY);
            ESP_LOGI(TAG, "WiFi ready");
        }

        if (!makapix_channel_is_sd_available()) {
            ESP_LOGI(TAG, "Waiting for SD card...");
            makapix_channel_wait_for_sd(portMAX_DELAY);
            ESP_LOGI(TAG, "SD card available");
        }

        // Wait if SDIO bus is locked or SD access is paused
        int wait_count = 0;
        const int max_wait = 120;  // Wait up to 120 seconds
        bool sdio_locked = sdio_bus_is_locked();
        bool sd_paused = (animation_player_is_sd_paused && animation_player_is_sd_paused());
        if (sdio_locked || sd_paused) {
            ESP_LOGD(TAG, "Waiting for bus/SD (sdio_locked=%d, sd_paused=%d)", sdio_locked, sd_paused);
        }
        while (wait_count < max_wait) {
            bool should_wait = sdio_bus_is_locked();
            if (!should_wait && animation_player_is_sd_paused) {
                should_wait = animation_player_is_sd_paused();
            }
            if (!should_wait) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_count++;
        }

        if (wait_count >= max_wait) {
            const char *holder = sdio_bus_get_holder();
            ESP_LOGW(TAG, "SDIO bus still locked by %s after %d seconds, skipping download cycle",
                     holder ? holder : "unknown", max_wait);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;  // Skip this cycle
        }

        // Run one time-budgeted slice of LAi verification batches when a
        // sweep is pending or active (pick-miss triggered reconciliation,
        // see lai_verify.h). Hosted here so verification shares this task's
        // gates and never contends with a second background SD scanner; the
        // ~500 ms slice keeps the sweep fast even when iterations are
        // dominated by multi-second downloads, while downloads stay active.
        lai_verify_result_t verify_state = LAI_VERIFY_IDLE;
        if (lai_verify_has_work()) {
            verify_state = lai_verify_run_slice();
        }

        // Resolve one pending museum entry per iteration. For Rijks
        // channels every cache entry arrives with extension=0xFF; the
        // resolver runs the 3-hop Linked-Art walk and mutates the entry
        // into a downloadable form (extension=3, iiif_key=micrio short
        // id). Cheap no-op for AIC channels (resolve_entry==NULL) and
        // for non-institution channels.
        art_institution_resolve_pending();

        // Clear any leftover cancel flag from a prior iteration. If
        // download_manager_set_channels set it while the previous
        // download was returning, the in-flight downloader has already
        // bailed by the time we get here; the next download we're about
        // to start belongs to the new channel set and must run.
        s_dl_cancel_requested = false;

        // Get next file to download using own round-robin logic
        // Take a snapshot of channel state under mutex to avoid race conditions
        // Note: Using static buffers (s_dl_req, s_dl_snapshot) to reduce stack usage
        memset(&s_dl_req, 0, sizeof(s_dl_req));
        esp_err_t get_err = ESP_ERR_NOT_FOUND;

        memset(&s_dl_snapshot, 0, sizeof(s_dl_snapshot));
        bool snapshot_ok = dl_take_snapshot(&s_dl_snapshot);
        ESP_LOGD(TAG, "snapshot_ok=%d, channel_count=%zu", snapshot_ok, s_dl_channel_count);
        size_t unresolved_count = 0;
        if (snapshot_ok) {
            get_err = dl_get_next_download(&s_dl_req, &s_dl_snapshot, &unresolved_count);
            ESP_LOGD(TAG, "dl_get_next_download returned %s (post_id=%ld)",
                     esp_err_to_name(get_err), (long)s_dl_req.post_id);
            if (get_err == ESP_OK) {
                // Clear the signal since we have work - prevents race condition where
                // signal is set while we're busy and gets cleared before we see it
                makapix_channel_clear_downloads_needed();
            }
        }

        if (get_err == ESP_ERR_NOT_FOUND) {
            // Sweep actively progressing: skip the wait and come back for
            // the next slice (pacing lives inside lai_verify_run_slice).
            if (verify_state == LAI_VERIFY_RAN) {
                continue;
            }
            // All files downloaded OR no channels configured - wait for signal
            // Clear signal before waiting so we only wake on NEW signals
            if (s_dl_channel_count == 0) {
                ESP_LOGD(TAG, "No channels configured, waiting for signal...");
            } else if (!s_all_downloaded_logged) {
                if (unresolved_count > 0) {
                    ESP_LOGI(TAG, "All resolvable entries downloaded (unresolved=%zu, ch_count=%zu), waiting for signal...",
                             unresolved_count, s_dl_channel_count);
                } else {
                    ESP_LOGI(TAG, "All files downloaded (ch_count=%zu), waiting for signal...", s_dl_channel_count);
                }
                s_all_downloaded_logged = true;
            }
            makapix_channel_clear_downloads_needed();
            // Bounded wait while verify work remains queued (a gated sweep
            // gets no wake signal when its gates clear, so poll). Re-checking
            // AFTER the clear also closes the race where a sweep request
            // lands between the top-of-loop check and the clear.
            makapix_channel_wait_for_downloads_needed(
                lai_verify_has_work() ? 5000 : portMAX_DELAY);
            ESP_LOGD(TAG, "Woke from downloads_needed wait");
            continue;
        }

        if (get_err != ESP_OK) {
            // Error getting next file
            ESP_LOGW(TAG, "Error getting next download: %s", esp_err_to_name(get_err));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Validate request
        if (s_dl_req.storage_key[0] == '\0' || s_dl_req.art_url[0] == '\0') {
            ESP_LOGW(TAG, "Invalid download request (empty storage_key or url)");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Check if file already exists (e.g., from a previous session, or just
        // merged into Ci by a refresh while the file was already on disk).
        // Treat this the same as a successful download — update LAi via the
        // play scheduler (which fires the zero-to-one transition if this is
        // the first available artwork) and continue to the next entry.
        if (file_exists(s_dl_req.filepath)) {
            ESP_LOGI(TAG, "File already exists, updating LAi: %s", s_dl_req.storage_key);
            s_all_downloaded_logged = false;

            // play_scheduler_on_download_complete -> ps_lai_add does the
            // zero-to-one playback trigger; the download manager no longer
            // calls play_scheduler_next() directly here (S2).
            play_scheduler_on_download_complete(s_dl_req.channel_id, s_dl_req.post_id);

            // Signal that a file is available (wakes tasks waiting for first playable file)
            makapix_channel_signal_file_available();

            vTaskDelay(pdMS_TO_TICKS(10));  // Brief delay, then check next file
            continue;
        }

        // Delete any existing temp file (using static buffer)
        if (s_dl_req.filepath[0]) {
            snprintf(s_task_temp_path, sizeof(s_task_temp_path), "%s.tmp", s_dl_req.filepath);
            struct stat tmp_st;
            if (stat(s_task_temp_path, &tmp_st) == 0) {
                ESP_LOGD(TAG, "Removing orphan temp file: %s", s_task_temp_path);
                unlink(s_task_temp_path);
            }
        }

        // Start download
        s_all_downloaded_logged = false;
        set_busy(true, s_dl_req.channel_id);

        // Update UI message while no animation is on screen yet. Once the
        // animation player has decoded and swapped to a buffer, it clears
        // this message itself, so the predicate naturally narrows after
        // the first successful playback.
        if (!animation_player_is_animation_ready()) {
            // Get display name from channel_id in the request (using static buffer)
            dl_get_display_name(s_dl_req.channel_id, s_task_display_name, sizeof(s_task_display_name));
            p3a_render_set_channel_message(s_task_display_name, 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, 0, "Downloading artwork...");
        }

        memset(s_task_out_path, 0, sizeof(s_task_out_path));
        ESP_LOGI(TAG, "Downloading: %s", s_dl_req.art_url);
        esp_err_t err;
        if (play_scheduler_is_giphy_channel(s_dl_req.channel_id)) {
            // Giphy channel: use giphy_download_artwork with the entry-aware
            // URL built in dl_get_next_download (and logged above), so the
            // per-entry downsized_medium override is honored.
            // Determine extension from the filepath
            uint8_t ext = 0;  // default webp
            size_t flen = strlen(s_dl_req.filepath);
            if (flen >= 4 && strcmp(s_dl_req.filepath + flen - 4, ".gif") == 0) ext = 1;
            if (!animation_player_is_animation_ready()) {
                err = giphy_download_artwork_with_progress(s_dl_req.storage_key, s_dl_req.art_url, ext,
                          s_task_out_path, sizeof(s_task_out_path),
                          dl_progress_cb, s_task_display_name);
            } else {
                err = giphy_download_artwork(s_dl_req.storage_key, s_dl_req.art_url, ext,
                          s_task_out_path, sizeof(s_task_out_path));
            }
        } else if (play_scheduler_is_institution_channel(s_dl_req.channel_id)) {
            // Institution channel: museum_id derived from spec_name; the URL
            // and target path were pre-built in dl_get_next_download.
            char ai_spec_name[33] = {0};
            char ai_museum_id[16] = {0};
            char ai_axis_unused[32] = {0};
            if (play_scheduler_get_channel_spec_name(s_dl_req.channel_id,
                                                    ai_spec_name, sizeof(ai_spec_name)) == ESP_OK) {
                art_institution_parse_spec(ai_spec_name,
                                           ai_museum_id, sizeof(ai_museum_id),
                                           ai_axis_unused, sizeof(ai_axis_unused));
            }
            err = art_institution_download_to_path(ai_museum_id, s_dl_req.art_url, s_dl_req.filepath);
            if (err == ESP_OK) {
                strlcpy(s_task_out_path, s_dl_req.filepath, sizeof(s_task_out_path));
            }
        } else {
            err = makapix_artwork_download(s_dl_req.art_url, s_dl_req.storage_key, s_task_out_path, sizeof(s_task_out_path));
        }

        set_busy(false, NULL);

        if (err == ESP_OK) {
            // Signal play_scheduler to update LAi using O(1) post_id lookup
            play_scheduler_on_download_complete(s_dl_req.channel_id, s_dl_req.post_id);

            makapix_channel_signal_downloads_needed();
            makapix_channel_signal_file_available();  // Wake tasks waiting for first file

            // S2: removed the redundant play_scheduler_next(NULL) call that
            // used to live here. play_scheduler_on_download_complete above
            // already drives the LAi zero-to-one transition (which emits
            // P3A_EVENT_SWAP_NEXT through the event bus), so the download
            // manager no longer needs its own playback-initiated flag.
        } else if (err == ESP_ERR_INVALID_STATE) {
            // ESP_ERR_INVALID_STATE has two cooperative-abort causes that share
            // the same handling: no 404 marker, no eviction, no .tmp left
            // behind. The next loop iteration will pick up wherever the new
            // state demands.
            //   (a) SD card got exported to USB MSC mid-stream.
            //   (b) Playset switched and our channel is no longer active
            //       (cooperative cancel via s_dl_cancel_requested).
            // We disambiguate by reading the cancel flag (read-only here; the
            // top-of-loop clear happens on the NEXT iteration).
            if (s_dl_cancel_requested) {
                ESP_LOGI(TAG, "Download canceled (playset switch): %s", s_dl_req.storage_key);
            } else {
                ESP_LOGI(TAG, "Download deferred (SD exported to USB): %s", s_dl_req.storage_key);
            }
            makapix_channel_signal_downloads_needed();
        } else {
            // Record failure with error classification for backoff
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
                // Keep .404 marker for fast stat() checks
                snprintf(s_task_marker_path, sizeof(s_task_marker_path), "%s.404", s_dl_req.filepath);
                FILE *f = fopen(s_task_marker_path, "w");
                if (f) {
                    time_t now = time(NULL);
                    fprintf(f, "%ld\n", (long)now);
                    fclose(f);
                    ESP_LOGD(TAG, "Created 404 marker: %s", s_task_marker_path);
                }
            } else {
                // Non-404 failure (network error, disk full, etc.) — reclaim space if low
                storage_eviction_check_and_run();
            }

            // Signal to check for next file immediately
            makapix_channel_signal_downloads_needed();
        }

        // Brief delay between downloads to reduce SDIO bus contention
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t download_manager_init(void)
{
    if (s_task) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    // The LAi verification sweep runs inside the download task loop
    lai_verify_init();

    const size_t stack_size = 81920;

    // Try PSRAM first for the 80KB stack
    s_download_stack = heap_caps_malloc(stack_size * sizeof(StackType_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_download_stack) {
        s_download_stack_allocated = true;
        // Pin to Core 0 to avoid interfering with animation rendering on Core 1
        s_task = xTaskCreateStaticPinnedToCore(download_task, "download_mgr",
                                   stack_size, NULL, 3,
                                   s_download_stack, &s_download_task_buffer, 0);
        if (s_task) {
            ESP_LOGI(TAG, "Download manager task using PSRAM stack (Core 0)");
            return ESP_OK;
        }
        heap_caps_free(s_download_stack);
        s_download_stack = NULL;
        s_download_stack_allocated = false;
    }

    // Fallback to dynamic allocation (internal RAM)
    // Pin to Core 0 to avoid interfering with animation rendering on Core 1
    ESP_LOGW(TAG, "PSRAM stack unavailable, using internal RAM");
    if (xTaskCreatePinnedToCore(download_task, "download_mgr", stack_size, NULL, 3, &s_task, 0) != pdPASS) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void download_manager_deinit(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_download_stack_allocated && s_download_stack) {
        heap_caps_free(s_download_stack);
        s_download_stack = NULL;
        s_download_stack_allocated = false;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

void download_manager_set_next_callback(download_get_next_cb_t cb, void *user_ctx)
{
    // Legacy API - no longer used, downloads are now driven by Play Scheduler
    // Kept for API compatibility
    (void)cb;
    (void)user_ctx;

    // If callback is set, signal that we should check for downloads
    if (cb) {
        makapix_channel_signal_downloads_needed();
    }
}

void download_manager_wake(void)
{
    // Just wake the download task without resetting any state.
    // Use this for single file re-downloads or retry after failures.
    makapix_channel_signal_downloads_needed();
}

void download_manager_rescan(void)
{
    // Reset cursors AND wake the download task to rescan from beginning.
    // Use this ONLY when new content has been added to the channel index.
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_dl_epoch++;
        ESP_LOGD(TAG, "rescan: resetting cursors for %zu channel(s)", s_dl_channel_count);
        for (size_t i = 0; i < s_dl_channel_count; i++) {
            s_dl_channels[i].dl_cursor = 0;
            s_dl_channels[i].scan_epoch_start = 0;
            s_dl_channels[i].has_wrapped = false;
            s_dl_channels[i].channel_complete = false;
        }
        xSemaphoreGive(s_mutex);
    }

    makapix_channel_signal_downloads_needed();
}

bool download_manager_is_busy(void)
{
    bool busy = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        busy = s_busy;
        xSemaphoreGive(s_mutex);
    } else {
        busy = s_busy;
    }
    return busy;
}

bool download_manager_get_active_channel(char *out_channel_id, size_t max_len)
{
    if (!out_channel_id || max_len == 0) return false;

    bool has_active = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active_channel[0]) {
            strlcpy(out_channel_id, s_active_channel, max_len);
            has_active = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return has_active;
}

// ============================================================================
// Decoupled Channel Management API
// ============================================================================

void download_manager_set_channels(const char **channel_ids, size_t count)
{
    bool cancel_in_flight = false;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (count > DL_MAX_CHANNELS) {
            ESP_LOGW(TAG, "Channel count %zu exceeds DL_MAX_CHANNELS (%d), truncated",
                     count, DL_MAX_CHANNELS);
        }
        s_dl_channel_count = (count < DL_MAX_CHANNELS) ? count : DL_MAX_CHANNELS;
        s_dl_round_robin_idx = 0;
        s_dl_epoch++;

        for (size_t i = 0; i < s_dl_channel_count; i++) {
            strlcpy(s_dl_channels[i].channel_id, channel_ids[i], sizeof(s_dl_channels[i].channel_id));
            s_dl_channels[i].dl_cursor = 0;
            s_dl_channels[i].scan_epoch_start = 0;
            s_dl_channels[i].has_wrapped = false;
            s_dl_channels[i].channel_complete = false;
        }

        // S1: if a download is in flight for a channel that's no longer in
        // the new set, request that it bail. Each downloader polls the
        // cancel flag between chunks and returns ESP_ERR_INVALID_STATE.
        // Without this, a multi-MB Giphy GIF download on the OLD playset
        // would block the user's freshly-picked playset for up to one full
        // transfer.
        if (s_busy && s_active_channel[0] != '\0') {
            bool still_active = false;
            for (size_t i = 0; i < s_dl_channel_count; i++) {
                if (strcmp(s_dl_channels[i].channel_id, s_active_channel) == 0) {
                    still_active = true;
                    break;
                }
            }
            if (!still_active) {
                cancel_in_flight = true;
            }
        }

        s_all_downloaded_logged = false;
        ESP_LOGI(TAG, "Configured %zu channel(s) for download%s",
                 s_dl_channel_count,
                 cancel_in_flight ? " (cancelling in-flight download of dropped channel)" : "");
        xSemaphoreGive(s_mutex);
    }

    if (cancel_in_flight) {
        s_dl_cancel_requested = true;
    }

    // Signal that we should check for downloads
    makapix_channel_signal_downloads_needed();
}

bool download_manager_is_canceled(void)
{
    return s_dl_cancel_requested;
}

void download_manager_reset_cursors(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_dl_epoch++;
        for (size_t i = 0; i < s_dl_channel_count; i++) {
            s_dl_channels[i].dl_cursor = 0;
            s_dl_channels[i].scan_epoch_start = 0;
            s_dl_channels[i].has_wrapped = false;
            s_dl_channels[i].channel_complete = false;
        }
        // Deliberately do NOT reset s_dl_round_robin_idx here. This function
        // runs on every refresh completion; snapping the round-robin back to
        // channel 0 each time made channel[0] monopolize downloads for the
        // whole refresh storm of a large playset (e.g. 64 sequential Giphy
        // refreshes ≈ one reset every ~7 s) while high-index channels
        // starved. Per-channel cursors must rescan from 0 (a merge can
        // insert entries anywhere), but fairness across channels survives a
        // rescan — matching download_manager_rescan(), which also preserves
        // the index. set_channels() still resets it: a new channel list is a
        // genuinely new cycle.
        s_all_downloaded_logged = false;
        ESP_LOGI(TAG, "Reset download cursors");
        xSemaphoreGive(s_mutex);
    }
}
