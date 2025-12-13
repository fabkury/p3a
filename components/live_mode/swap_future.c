#include "swap_future.h"
#include "play_navigator.h"
#include "sync_playlist.h"
#include "sntp_sync.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>

static const char *TAG = "swap_future";

// External: wakes the auto_swap_task so it can re-evaluate timing.
extern void auto_swap_reset_timer(void);

static uint64_t wall_clock_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// Live Mode recovery state (process-wide; single active channel at a time in p3a)
static struct {
    bool active;
    uint64_t until_ms;          // when to attempt resync (end of fallback dwell)
    uint32_t consecutive_failures;
    uint64_t backoff_until_ms;
} s_live_recovery = {0};

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

static esp_err_t resolve_live_artwork(play_navigator_t *nav, uint32_t live_idx, artwork_ref_t *out)
{
    if (!nav || !out) return ESP_ERR_INVALID_ARG;
    if (!nav->live_ready || !nav->live_p || !nav->live_q || live_idx >= nav->live_count) {
        return ESP_ERR_INVALID_STATE;
    }

    play_navigator_t tmp = *nav;
    tmp.live_mode = false;
    tmp.p = tmp.live_p[live_idx];
    tmp.q = tmp.live_q[live_idx];
    return play_navigator_current(&tmp, out);
}

static esp_err_t schedule_swap_for_live_index(play_navigator_t *nav,
                                              uint32_t live_idx,
                                              uint64_t target_time_ms,
                                              uint64_t start_time_ms)
{
    artwork_ref_t art = {0};
    esp_err_t e = resolve_live_artwork(nav, live_idx, &art);
    if (e != ESP_OK) return e;

    swap_future_t sf = {0};
    sf.valid = true;
    sf.target_time_ms = target_time_ms;
    sf.start_time_ms = start_time_ms;
    sf.start_frame = 0;
    sf.live_index = live_idx;
    sf.artwork = art;
    sf.is_live_mode_swap = true;
    sf.is_automated = true;

    return swap_future_schedule(&sf);
}

// Global state for swap_future system
static struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    swap_future_t pending;
} s_swap_future = {0};

esp_err_t swap_future_init(void)
{
    if (s_swap_future.initialized) {
        ESP_LOGW(TAG, "swap_future already initialized");
        return ESP_OK;
    }
    
    // Create mutex for thread-safe access
    s_swap_future.mutex = xSemaphoreCreateMutex();
    if (!s_swap_future.mutex) {
        ESP_LOGE(TAG, "Failed to create swap_future mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize pending swap as invalid
    memset(&s_swap_future.pending, 0, sizeof(swap_future_t));
    s_swap_future.pending.valid = false;
    
    s_swap_future.initialized = true;
    ESP_LOGI(TAG, "swap_future system initialized");
    
    return ESP_OK;
}

void swap_future_deinit(void)
{
    if (!s_swap_future.initialized) {
        return;
    }
    
    if (s_swap_future.mutex) {
        vSemaphoreDelete(s_swap_future.mutex);
        s_swap_future.mutex = NULL;
    }
    
    memset(&s_swap_future.pending, 0, sizeof(swap_future_t));
    s_swap_future.initialized = false;
    
    ESP_LOGI(TAG, "swap_future system deinitialized");
}

esp_err_t swap_future_schedule(const swap_future_t *swap)
{
    if (!s_swap_future.initialized) {
        ESP_LOGE(TAG, "swap_future system not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!swap) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for scheduling");
        return ESP_ERR_TIMEOUT;
    }
    
    // Copy the swap_future
    memcpy(&s_swap_future.pending, swap, sizeof(swap_future_t));
    s_swap_future.pending.valid = true;
    
    ESP_LOGI(TAG, "Scheduled swap_future: target=%llu ms, start=%llu ms, frame=%u, live=%d, auto=%d",
             (unsigned long long)swap->target_time_ms,
             (unsigned long long)swap->start_time_ms,
             (unsigned)swap->start_frame,
             swap->is_live_mode_swap, swap->is_automated);
    
    xSemaphoreGive(s_swap_future.mutex);
    
    return ESP_OK;
}

void swap_future_cancel(void)
{
    if (!s_swap_future.initialized) {
        return;
    }
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for cancellation");
        return;
    }
    
    if (s_swap_future.pending.valid) {
        ESP_LOGI(TAG, "Cancelled pending swap_future");
        s_swap_future.pending.valid = false;
    }
    
    xSemaphoreGive(s_swap_future.mutex);
}

bool swap_future_is_ready(uint64_t current_time_ms, swap_future_t *out_swap)
{
    if (!s_swap_future.initialized) {
        return false;
    }
    
    bool ready = false;
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_swap_future.pending.valid && 
            current_time_ms >= s_swap_future.pending.target_time_ms) {
            ready = true;
            
            if (out_swap) {
                memcpy(out_swap, &s_swap_future.pending, sizeof(swap_future_t));
            }
        }
        
        xSemaphoreGive(s_swap_future.mutex);
    }
    
    return ready;
}

esp_err_t swap_future_get_pending(swap_future_t *out_swap)
{
    if (!s_swap_future.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!out_swap) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t result = ESP_ERR_NOT_FOUND;
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_swap_future.pending.valid) {
            memcpy(out_swap, &s_swap_future.pending, sizeof(swap_future_t));
            result = ESP_OK;
        }
        
        xSemaphoreGive(s_swap_future.mutex);
    }
    
    return result;
}

bool swap_future_has_pending(void)
{
    if (!s_swap_future.initialized) {
        return false;
    }
    
    bool has_pending = false;
    
    if (xSemaphoreTake(s_swap_future.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        has_pending = s_swap_future.pending.valid;
        xSemaphoreGive(s_swap_future.mutex);
    }
    
    return has_pending;
}

esp_err_t live_mode_enter(void *navigator)
{
    if (!navigator) {
        return ESP_ERR_INVALID_ARG;
    }
    
    play_navigator_t *nav = (play_navigator_t *)navigator;
    
    // Check precondition: NTP must be synchronized
    if (!sntp_sync_is_synchronized()) {
        ESP_LOGW(TAG, "Cannot enter Live Mode: NTP not synchronized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Entering Live Mode");
    
    // Enable Live Mode in navigator (will rebuild schedule if needed)
    play_navigator_set_live_mode(nav, true);
    
    // Force schedule build + compute current position and elapsed.
    artwork_ref_t current_art = {0};
    esp_err_t cur_err = play_navigator_current(nav, &current_art);
    if (cur_err != ESP_OK) {
        ESP_LOGW(TAG, "Live Mode enter: failed to resolve current artwork: %s", esp_err_to_name(cur_err));
        return cur_err;
    }

    uint32_t cur_idx = 0;
    uint32_t elapsed_ms = 0;
    uint64_t now_ms = wall_clock_ms();
    (void)SyncPlaylist.update(now_ms, &cur_idx, &elapsed_ms);

    // Schedule an immediate swap_future that starts at the correct elapsed offset.
    swap_future_t entry = {0};
    entry.valid = true;
    entry.target_time_ms = now_ms;                 // execute ASAP
    entry.start_time_ms = now_ms - (uint64_t)elapsed_ms; // ideal start
    entry.start_frame = 0;                         // prefer start_time_ms seeking
    entry.live_index = cur_idx;
    entry.artwork = current_art;
    entry.is_live_mode_swap = true;
    entry.is_automated = true;

    esp_err_t sch_err = swap_future_schedule(&entry);
    if (sch_err != ESP_OK) {
        ESP_LOGW(TAG, "Live Mode enter: failed to schedule entry swap_future: %s", esp_err_to_name(sch_err));
        return sch_err;
    }

    // Wake auto-swap task so it can execute immediately.
    auto_swap_reset_timer();
    
    ESP_LOGI(TAG, "Live Mode entered successfully");
    
    return ESP_OK;
}

void live_mode_exit(void *navigator)
{
    if (!navigator) {
        return;
    }
    
    play_navigator_t *nav = (play_navigator_t *)navigator;
    
    ESP_LOGI(TAG, "Exiting Live Mode");
    
    // Disable Live Mode in navigator
    play_navigator_set_live_mode(nav, false);
    
    // Cancel any pending swap_future
    swap_future_cancel();

    // Clear recovery state
    memset(&s_live_recovery, 0, sizeof(s_live_recovery));
    
    ESP_LOGI(TAG, "Live Mode exited");
}

bool live_mode_is_active(void *navigator)
{
    if (!navigator) {
        return false;
    }
    
    play_navigator_t *nav = (play_navigator_t *)navigator;
    return nav->live_mode;
}

esp_err_t live_mode_schedule_next_swap(void *navigator)
{
    if (!navigator) {
        return ESP_ERR_INVALID_ARG;
    }

    play_navigator_t *nav = (play_navigator_t *)navigator;
    if (!nav->live_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sntp_sync_is_synchronized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (swap_future_has_pending()) {
        return ESP_OK;
    }

    // Ensure schedule exists and navigator is at the correct current position.
    artwork_ref_t cur_art = {0};
    esp_err_t cur_err = play_navigator_current(nav, &cur_art);
    if (cur_err != ESP_OK) {
        return cur_err;
    }

    const uint64_t now_ms = wall_clock_ms();

    // Recovery mode: attempt to re-sync at the end of the fallback dwell window.
    if (s_live_recovery.active) {
        if (now_ms < s_live_recovery.until_ms) {
            uint32_t idx = 0;
            uint32_t elapsed = 0;
            (void)SyncPlaylist.update(s_live_recovery.until_ms + 1, &idx, &elapsed);

            esp_err_t sch = schedule_swap_for_live_index(nav, idx, s_live_recovery.until_ms,
                                                         (s_live_recovery.until_ms + 1) - (uint64_t)elapsed);
            if (sch == ESP_OK) {
                auto_swap_reset_timer();
            }
            return sch;
        }

        // Past the recovery boundary: resume normal scheduling.
        s_live_recovery.active = false;
        s_live_recovery.until_ms = 0;
    }

    uint32_t cur_idx = 0;
    uint32_t elapsed_ms = 0;
    (void)SyncPlaylist.update(now_ms, &cur_idx, &elapsed_ms);

    uint32_t dur_ms = 0;
    if (sync_playlist_get_duration_ms(cur_idx, &dur_ms) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t cur_start_ms = now_ms - (uint64_t)elapsed_ms;
    uint64_t next_start_ms = cur_start_ms + (uint64_t)dur_ms;

    uint32_t next_idx = 0;
    uint32_t next_elapsed = 0;
    (void)SyncPlaylist.update(next_start_ms + 1, &next_idx, &next_elapsed);

    esp_err_t sch_err = schedule_swap_for_live_index(nav, next_idx, next_start_ms, next_start_ms);
    if (sch_err == ESP_OK) {
        auto_swap_reset_timer();
    }
    return sch_err;
}

esp_err_t live_mode_recover_from_failed_swap(void *navigator, uint32_t failed_live_index, esp_err_t reason)
{
    if (!navigator) return ESP_ERR_INVALID_ARG;

    play_navigator_t *nav = (play_navigator_t *)navigator;
    if (!nav->live_mode) return ESP_ERR_INVALID_STATE;
    if (!sntp_sync_is_synchronized()) return ESP_ERR_INVALID_STATE;

    const uint64_t now_ms = wall_clock_ms();
    if (s_live_recovery.backoff_until_ms != 0 && now_ms < s_live_recovery.backoff_until_ms) {
        ESP_LOGW(TAG, "Live recovery backoff active (%llu ms remaining)",
                 (unsigned long long)(s_live_recovery.backoff_until_ms - now_ms));
        return ESP_ERR_TIMEOUT;
    }

    // Ensure live schedule exists.
    artwork_ref_t dummy = {0};
    esp_err_t cur_err = play_navigator_current(nav, &dummy);
    if (cur_err != ESP_OK) return cur_err;
    if (!nav->live_ready || nav->live_count == 0) return ESP_ERR_INVALID_STATE;

    const uint32_t live_count = nav->live_count;
    const uint32_t start = (failed_live_index < live_count) ? ((failed_live_index + 1) % live_count) : 0;

    const uint32_t scan_max = (live_count < 32u) ? live_count : 32u;

    for (uint32_t off = 0; off < scan_max; off++) {
        uint32_t idx = (start + off) % live_count;
        artwork_ref_t art = {0};
        if (resolve_live_artwork(nav, idx, &art) != ESP_OK) {
            continue;
        }
        if (!file_exists(art.filepath)) {
            continue;
        }

        // Schedule an immediate attempt to swap to this candidate.
        swap_future_cancel();
        esp_err_t sch = schedule_swap_for_live_index(nav, idx, now_ms, 0);
        if (sch != ESP_OK) {
            continue;
        }

        // Recovery window ends after this candidate's dwell; then we attempt to re-sync.
        uint32_t dur_ms = 0;
        if (sync_playlist_get_duration_ms(idx, &dur_ms) != ESP_OK || dur_ms == 0) {
            dur_ms = 30000;
        }
        s_live_recovery.active = true;
        s_live_recovery.until_ms = now_ms + (uint64_t)dur_ms;
        s_live_recovery.consecutive_failures = 0;
        s_live_recovery.backoff_until_ms = 0;

        ESP_LOGW(TAG, "Live Mode swap failed (%s). Skipping to candidate idx=%u for %u ms then re-sync.",
                 esp_err_to_name(reason), (unsigned)idx, (unsigned)dur_ms);

        auto_swap_reset_timer();
        return ESP_OK;
    }

    // Nothing found in scan window: apply exponential backoff and try again later via normal scheduling.
    s_live_recovery.consecutive_failures++;
    uint32_t k = s_live_recovery.consecutive_failures;
    if (k > 7) k = 7;
    uint64_t backoff_ms = 250ULL * (1ULL << k); // 500ms, 1s, 2s, ... up to 32s (capped below)
    if (backoff_ms > 30000ULL) backoff_ms = 30000ULL;
    s_live_recovery.backoff_until_ms = now_ms + backoff_ms;

    ESP_LOGW(TAG, "Live Mode recovery: no candidate found in next %u items; backoff %llu ms (reason=%s)",
             (unsigned)scan_max, (unsigned long long)backoff_ms, esp_err_to_name(reason));

    // Schedule a retry at backoff time to the then-current scheduled item.
    uint32_t idx = 0;
    uint32_t elapsed = 0;
    (void)SyncPlaylist.update(s_live_recovery.backoff_until_ms + 1, &idx, &elapsed);
    swap_future_cancel();
    esp_err_t sch = schedule_swap_for_live_index(nav, idx, s_live_recovery.backoff_until_ms,
                                                 (s_live_recovery.backoff_until_ms + 1) - (uint64_t)elapsed);
    if (sch == ESP_OK) {
        auto_swap_reset_timer();
    }
    return sch;
}

void live_mode_notify_swap_succeeded(uint32_t live_index)
{
    (void)live_index;
    s_live_recovery.consecutive_failures = 0;
    s_live_recovery.backoff_until_ms = 0;
}
