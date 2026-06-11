// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_refresh.c
 * @brief Background channel index refresh for Play Scheduler
 */

#include "makapix_internal.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// Background channel index refresh (for Play Scheduler)
// ---------------------------------------------------------------------------

// Track background refresh handles to avoid recreating them repeatedly
static channel_handle_t s_refresh_handle_all = NULL;
static channel_handle_t s_refresh_handle_promoted = NULL;

// Track user/hashtag refresh handles for cancellation.
//
// THREADING: s_tracked_refresh_handles[] and s_tracked_refresh_count are read
// and modified from at least two contexts — the ps_refresh task (via
// makapix_refresh_channel_index) and whichever task drives playset switches
// (via makapix_cancel_all_refreshes). Without serialization, one thread can
// pull a handle out of the array and channel_destroy() it while another is
// mid-iteration on the same array, leaving the second thread with a dangling
// pointer. The trailing stop_refresh()/reap then takes a freed semaphore and
// hangs the spinlock acquire (interrupt watchdog timeout).
//
// Rule: hold s_tracked_mutex while reading or writing the array or the count.
// Never call stop_refresh() or channel_destroy() with the mutex held — both
// can take seconds. The pattern is: snapshot under the mutex, clear the slot
// under the mutex, release the mutex, then destroy. After the slot is
// cleared, no other thread can find that handle in the array.
#define MAX_TRACKED_REFRESH_HANDLES PS_MAX_CHANNELS
static channel_handle_t s_tracked_refresh_handles[MAX_TRACKED_REFRESH_HANDLES] = {NULL};
static size_t s_tracked_refresh_count = 0;
static SemaphoreHandle_t s_tracked_mutex = NULL;

static SemaphoreHandle_t tracked_mutex(void)
{
    static StaticSemaphore_t s_mutex_buf;
    if (!s_tracked_mutex) {
        s_tracked_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    }
    return s_tracked_mutex;
}

// ---------------------------------------------------------------------------
// Cancel all active refresh tasks
// ---------------------------------------------------------------------------

esp_err_t makapix_cancel_all_refreshes(void)
{
    ESP_LOGI(MAKAPIX_TAG, "Cancelling all active refresh tasks");

    // Static handles aren't tracked here — they live for the lifetime of the
    // process and we only stop them, never destroy them, so there's no
    // dangling-pointer race to worry about.
    if (s_refresh_handle_all) {
        makapix_channel_stop_refresh(s_refresh_handle_all);
    }
    if (s_refresh_handle_promoted) {
        makapix_channel_stop_refresh(s_refresh_handle_promoted);
    }

    // Snapshot the tracked array under the mutex, then clear it. After this
    // critical section no other thread can reach these handles, so we can
    // stop+destroy them outside the lock without racing with concurrent
    // makapix_refresh_channel_index() callers.
    channel_handle_t snapshot[MAX_TRACKED_REFRESH_HANDLES];
    size_t snapshot_count = 0;
    xSemaphoreTake(tracked_mutex(), portMAX_DELAY);
    snapshot_count = s_tracked_refresh_count;
    if (snapshot_count > 0) {
        memcpy(snapshot, s_tracked_refresh_handles,
               snapshot_count * sizeof(channel_handle_t));
    }
    memset(s_tracked_refresh_handles, 0, sizeof(s_tracked_refresh_handles));
    s_tracked_refresh_count = 0;
    xSemaphoreGive(tracked_mutex());

    for (size_t i = 0; i < snapshot_count; i++) {
        if (snapshot[i]) {
            makapix_channel_stop_refresh(snapshot[i]);
            channel_destroy(snapshot[i]);
        }
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Eager reap of finished refresh tasks
// ---------------------------------------------------------------------------

esp_err_t makapix_reap_finished_refresh(const char *channel_id)
{
    if (!channel_id) return ESP_ERR_INVALID_ARG;

    // Snapshot the matching handle under the mutex, then call reap outside
    // the mutex (reap can block for tens of ms waiting for the task to
    // self-suspend, and the lock protects array slots, not handle lifetime).
    // The handle itself is not destroyed by reap — it stays in the tracked
    // array and will be reused (or evicted) by the next refresh request.
    channel_handle_t target = NULL;

    if (s_refresh_handle_all &&
        strcmp(makapix_channel_get_id(s_refresh_handle_all), channel_id) == 0) {
        target = s_refresh_handle_all;
    } else if (s_refresh_handle_promoted &&
               strcmp(makapix_channel_get_id(s_refresh_handle_promoted), channel_id) == 0) {
        target = s_refresh_handle_promoted;
    } else {
        xSemaphoreTake(tracked_mutex(), portMAX_DELAY);
        for (size_t i = 0; i < s_tracked_refresh_count; i++) {
            channel_handle_t h = s_tracked_refresh_handles[i];
            if (h && strcmp(makapix_channel_get_id(h), channel_id) == 0) {
                target = h;
                break;
            }
        }
        xSemaphoreGive(tracked_mutex());
    }

    if (!target) return ESP_OK;  // Not a tracked refresh — nothing to reap
    return makapix_channel_reap_if_finished(target);
}

// ---------------------------------------------------------------------------
// Play Scheduler refresh completion tracking
// ---------------------------------------------------------------------------

#define MAX_PS_PENDING_REFRESH PS_MAX_CHANNELS

typedef struct {
    char channel_id[64];
    char display_name[64];
    bool completed;
    bool succeeded;  // walk ran to its end (vs cancelled / failed mid-walk)
} ps_refresh_pending_t;

static ps_refresh_pending_t s_ps_pending_refreshes[MAX_PS_PENDING_REFRESH] = {0};
static SemaphoreHandle_t s_ps_pending_mutex = NULL;

// Resolve a display name for logs given a channel_id. Caller-provided name wins;
// otherwise look up via the scheduler; otherwise fall back to the raw channel_id.
static void resolve_display_name(const char *channel_id, const char *provided,
                                 char *out, size_t out_len)
{
    if (provided && provided[0] != '\0') {
        strlcpy(out, provided, out_len);
        return;
    }
    ps_get_display_name(channel_id, out, out_len);
}

void makapix_ps_refresh_register(const char *channel_id, const char *display_name)
{
    if (!channel_id) return;

    // Create mutex on first use
    if (!s_ps_pending_mutex) {
        s_ps_pending_mutex = xSemaphoreCreateMutex();
        if (!s_ps_pending_mutex) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create PS refresh mutex");
            return;
        }
    }

    char dn[64];
    resolve_display_name(channel_id, display_name, dn, sizeof(dn));

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    // Check if already registered
    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0) {
            // Already registered, reset completion flags and refresh display name
            s_ps_pending_refreshes[i].completed = false;
            s_ps_pending_refreshes[i].succeeded = false;
            strlcpy(s_ps_pending_refreshes[i].display_name, dn,
                    sizeof(s_ps_pending_refreshes[i].display_name));
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGD(MAKAPIX_TAG, "PS refresh re-registered: %s", dn);
            return;
        }
    }

    // Find empty slot
    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (s_ps_pending_refreshes[i].channel_id[0] == '\0') {
            strlcpy(s_ps_pending_refreshes[i].channel_id, channel_id,
                    sizeof(s_ps_pending_refreshes[i].channel_id));
            strlcpy(s_ps_pending_refreshes[i].display_name, dn,
                    sizeof(s_ps_pending_refreshes[i].display_name));
            s_ps_pending_refreshes[i].completed = false;
            s_ps_pending_refreshes[i].succeeded = false;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGD(MAKAPIX_TAG, "PS refresh registered: %s", dn);
            return;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    ESP_LOGW(MAKAPIX_TAG, "PS refresh table full, cannot register: %s", dn);
}

void makapix_ps_refresh_mark_complete(const char *channel_id, bool succeeded)
{
    if (!channel_id || !s_ps_pending_mutex) return;

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0) {
            s_ps_pending_refreshes[i].completed = true;
            s_ps_pending_refreshes[i].succeeded = succeeded;
            char dn[64];
            resolve_display_name(channel_id, s_ps_pending_refreshes[i].display_name,
                                 dn, sizeof(dn));
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGI(MAKAPIX_TAG, "PS refresh complete: %s (%s)", dn,
                     succeeded ? "ok" : "incomplete");
            // Signal Play Scheduler
            makapix_channel_signal_ps_refresh_done(channel_id);
            return;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    // Not registered - that's OK, may have been triggered by non-PS path
}

bool makapix_ps_refresh_check_and_clear(const char *channel_id, bool *out_succeeded)
{
    if (!channel_id || !s_ps_pending_mutex) return false;

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0 &&
            s_ps_pending_refreshes[i].completed) {
            if (out_succeeded) {
                *out_succeeded = s_ps_pending_refreshes[i].succeeded;
            }
            // Clear the entry
            s_ps_pending_refreshes[i].channel_id[0] = '\0';
            s_ps_pending_refreshes[i].completed = false;
            s_ps_pending_refreshes[i].succeeded = false;
            xSemaphoreGive(s_ps_pending_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    return false;
}

// ---------------------------------------------------------------------------

esp_err_t makapix_refresh_channel_index(const char *channel_type, const char *identifier)
{
    if (!channel_type) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check MQTT connection
    if (!makapix_mqtt_is_connected()) {
        ESP_LOGW(MAKAPIX_TAG, "Cannot refresh channel: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    // Build channel_id from type and identifier using hash
    char channel_id[128] = {0};
    char channel_name[64] = {0};

    // Makapix channels do not carry per-playset offsets; the canonical channel
    // is offset=0.
    if (strcmp(channel_type, "all") == 0) {
        ps_compute_channel_id(PS_CHANNEL_TYPE_NAMED, "all", "", 0, channel_id, sizeof(channel_id));
        strlcpy(channel_name, "All", sizeof(channel_name));
    } else if (strcmp(channel_type, "promoted") == 0) {
        ps_compute_channel_id(PS_CHANNEL_TYPE_NAMED, "promoted", "", 0, channel_id, sizeof(channel_id));
        strlcpy(channel_name, "Promoted", sizeof(channel_name));
    } else if (strcmp(channel_type, "by_user") == 0 && identifier) {
        ps_compute_channel_id(PS_CHANNEL_TYPE_USER, "user", identifier, 0, channel_id, sizeof(channel_id));
        snprintf(channel_name, sizeof(channel_name), "User %s", identifier);
    } else if (strcmp(channel_type, "reactions") == 0 && identifier) {
        ps_compute_channel_id(PS_CHANNEL_TYPE_REACTIONS, "reactions", identifier, 0,
                              channel_id, sizeof(channel_id));
        snprintf(channel_name, sizeof(channel_name), "Reactions: %s", identifier);
    } else if (strcmp(channel_type, "hashtag") == 0 && identifier) {
        ps_compute_channel_id(PS_CHANNEL_TYPE_HASHTAG, "hashtag", identifier, 0, channel_id, sizeof(channel_id));
        snprintf(channel_name, sizeof(channel_name), "#%s", identifier);
    } else {
        ESP_LOGW(MAKAPIX_TAG, "Unknown channel type: %s", channel_type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(MAKAPIX_TAG, "Refreshing channel index: %s (no channel switch)", channel_name);

    // Register for Play Scheduler completion tracking
    makapix_ps_refresh_register(channel_id, channel_name);

    // Get paths
    char vault_path[128] = {0};
    char channels_path[128] = {0};
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        snprintf(vault_path, sizeof(vault_path), "%s/vault", SD_PATH_DEFAULT_ROOT);
    }
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        snprintf(channels_path, sizeof(channels_path), "%s/channel", SD_PATH_DEFAULT_ROOT);
    }

    // Check if we already have a handle for this channel type (reuse for "all" and "promoted")
    channel_handle_t handle = NULL;
    if (strcmp(channel_type, "all") == 0) {
        if (!s_refresh_handle_all) {
            s_refresh_handle_all = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
            if (s_refresh_handle_all) makapix_channel_set_spec(s_refresh_handle_all, channel_type, identifier);
        }
        handle = s_refresh_handle_all;
    } else if (strcmp(channel_type, "promoted") == 0) {
        if (!s_refresh_handle_promoted) {
            s_refresh_handle_promoted = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
            if (s_refresh_handle_promoted) makapix_channel_set_spec(s_refresh_handle_promoted, channel_type, identifier);
        }
        handle = s_refresh_handle_promoted;
    } else {
        // Look for an existing handle for this channel_id under the mutex.
        // If found and still refreshing, we're done. If found but stale,
        // pull it out of tracking and destroy it OUTSIDE the mutex so a
        // concurrent makapix_cancel_all_refreshes can't race with us on the
        // same slot (and so the slow stop+destroy doesn't block other
        // tracked-array readers).
        channel_handle_t stale = NULL;
        bool already_refreshing = false;
        xSemaphoreTake(tracked_mutex(), portMAX_DELAY);
        for (size_t i = 0; i < s_tracked_refresh_count; i++) {
            if (s_tracked_refresh_handles[i] &&
                strcmp(makapix_channel_get_id(s_tracked_refresh_handles[i]), channel_id) == 0) {
                if (makapix_channel_is_refreshing(s_tracked_refresh_handles[i])) {
                    already_refreshing = true;
                } else {
                    stale = s_tracked_refresh_handles[i];
                    s_tracked_refresh_handles[i] = s_tracked_refresh_handles[--s_tracked_refresh_count];
                    s_tracked_refresh_handles[s_tracked_refresh_count] = NULL;
                }
                break;
            }
        }
        xSemaphoreGive(tracked_mutex());

        if (already_refreshing) {
            ESP_LOGI(MAKAPIX_TAG, "Refresh already in progress for %s, skipping", channel_name);
            return ESP_OK;
        }
        if (stale) {
            ESP_LOGD(MAKAPIX_TAG, "Cleaning up stale refresh handle for %s", channel_name);
            makapix_channel_stop_refresh(stale);
            channel_destroy(stale);
        }

        // For user/hashtag channels, create a handle and track it for cancellation
        handle = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        if (handle) makapix_channel_set_spec(handle, channel_type, identifier);
        if (!handle) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create channel for refresh: %s", channel_name);
            return ESP_ERR_NO_MEM;
        }

        // Insert into tracking AND start the refresh task under the same
        // critical section. Splitting these would leave a window where the
        // handle is in the array but ch->refresh_task is still NULL — a
        // concurrent makapix_cancel_all_refreshes would then call
        // stop_refresh (which short-circuits on NULL refresh_task without
        // reaping) and channel_destroy, freeing the struct out from under
        // our pending channel_load. The "evicted oldest" handle is removed
        // from tracking under the lock but actually stop+destroyed outside,
        // since no other thread can see it once it's out of the array.
        channel_handle_t evicted = NULL;
        esp_err_t err;
        xSemaphoreTake(tracked_mutex(), portMAX_DELAY);
        if (s_tracked_refresh_count < MAX_TRACKED_REFRESH_HANDLES) {
            s_tracked_refresh_handles[s_tracked_refresh_count++] = handle;
        } else {
            evicted = s_tracked_refresh_handles[0];
            for (size_t i = 0; i < MAX_TRACKED_REFRESH_HANDLES - 1; i++) {
                s_tracked_refresh_handles[i] = s_tracked_refresh_handles[i + 1];
            }
            s_tracked_refresh_handles[MAX_TRACKED_REFRESH_HANDLES - 1] = handle;
        }

        err = channel_load(handle);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            // Pull the freshly-inserted handle back out of tracking before
            // releasing the mutex so no concurrent caller observes it.
            for (size_t i = 0; i < s_tracked_refresh_count; i++) {
                if (s_tracked_refresh_handles[i] == handle) {
                    s_tracked_refresh_handles[i] = s_tracked_refresh_handles[--s_tracked_refresh_count];
                    s_tracked_refresh_handles[s_tracked_refresh_count] = NULL;
                    break;
                }
            }
        }
        xSemaphoreGive(tracked_mutex());

        // Outside-the-lock cleanup. `evicted` and (on load failure) `handle`
        // are no longer in the tracked array, so no other thread can race
        // with these stop+destroy calls.
        if (evicted) {
            ESP_LOGW(MAKAPIX_TAG, "Refresh handle tracking full, evicted oldest");
            makapix_channel_stop_refresh(evicted);
            channel_destroy(evicted);
        } else {
            ESP_LOGD(MAKAPIX_TAG, "Tracking refresh handle for %s", channel_id);
        }

        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(MAKAPIX_TAG, "Channel load/refresh failed: %s", esp_err_to_name(err));
            channel_destroy(handle);
            return err;
        }
        return ESP_OK;
    }

    if (!handle) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create/get channel handle for refresh: %s", channel_name);
        return ESP_ERR_NO_MEM;
    }

    // Trigger refresh via channel_load (which calls request_refresh internally
    // and starts the refresh task if needed).
    //
    // Do NOT call channel_request_refresh again as a fallback. channel_load
    // returns ESP_OK even when its internal request_refresh failed (e.g. reap
    // timeout) — in that window ch->refresh_task is NULL and ch->refreshing is
    // false, so a second request_refresh would skip the reap, create a fresh
    // task, and orphan it from any pending cancellation.
    esp_err_t err = channel_load(handle);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(MAKAPIX_TAG, "Channel load/refresh failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(MAKAPIX_TAG, "Refresh initiated for %s (background)", channel_name);
    return ESP_OK;
}
