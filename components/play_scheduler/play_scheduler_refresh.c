// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file play_scheduler_refresh.c
 * @brief Background channel refresh task for Play Scheduler
 *
 * Sequentially refreshes channels that have `refresh_pending` set.
 * For SD card channels: rebuilds the sdcard.bin index
 * For Makapix channels: triggers the existing Makapix refresh mechanism
 */

#include "play_scheduler_internal.h"
#include "play_scheduler.h"  // For play_scheduler_next()
#include "channel_cache.h"   // For channel_cache_save()
#include "makapix.h"
#include "makapix_mqtt.h"     // For makapix_mqtt_is_connected()
#include "makapix_artwork.h"  // For makapix_artwork_download_with_progress()
#include "makapix_channel_events.h"  // For async completion events
#include "giphy.h"             // For giphy_refresh_channel()
#include "art_institution.h"   // For art_institution_refresh_by_spec()
#include "makapix_promoted_https.h"  // For HTTPS fallback refresh
#include "config_store.h"      // For config_store_get_giphy_refresh_interval()
#include "channel_metadata.h"  // For channel_metadata_load()
#include "sntp_sync.h"         // For sntp_sync_is_synchronized()
#include "sd_path.h"
#include "pin_lists.h"         // For pin_lists_gc_kick()
#include "p3a_state.h"   // For P3A_CHANNEL_MSG_* constants
#include "p3a_render.h"  // For p3a_render_set_channel_message()
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"   // For esp_timer_get_time() (failure backoff deadlines)
#include "esp_random.h"  // For esp_random() (backoff jitter)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "ps_refresh";

// Task configuration
#define REFRESH_TASK_STACK_SIZE 8192
#define REFRESH_CHECK_INTERVAL_MS 2000

// Periodic refresh configuration
#define REFRESH_INTERVAL_SECONDS 3600  // 1 hour
#define REFRESH_MIN_DELAY_SECONDS 10   // Floor to prevent tight loops

// Sentinel for channel_next_actionable_sec(): the channel contributes no timed
// deadline — it is either gated on a connectivity event (Wi-Fi/MQTT signals
// wake the task via ps_refresh_signal_work) or has no freshness interval.
#define NEXT_CHECK_NO_DEADLINE UINT32_MAX

// Per-channel failure retry backoff: 30 s doubling to a 15 min cap (shift
// clamped at use site so a long streak can't overflow), ±20% jitter applied
// when armed so channels that failed together don't retry in lockstep.
#define RETRY_BACKOFF_BASE_SEC 30
#define RETRY_BACKOFF_CAP_SEC  900

// Concurrency control
#define REFRESH_MAX_CONCURRENT 2

/**
 * @brief Compute async refresh timeout in milliseconds
 *
 * 60 seconds per 256 items in the max channel cache size (runtime-configurable),
 * with a floor of 60 seconds.  Larger caches take longer to refresh because each
 * MQTT page round-trip adds latency.
 */
static inline uint32_t refresh_async_timeout_ms(void)
{
    uint32_t max_entries = channel_cache_get_max_entries();
    uint32_t timeout = (max_entries / 256) * 60000;
    return timeout > 60000 ? timeout : 60000;
}

// Event bits
#define REFRESH_EVENT_WORK_AVAILABLE   (1 << 0)
#define REFRESH_EVENT_SHUTDOWN         (1 << 1)
#define REFRESH_EVENT_PARKED           (1 << 2)  // Set by task immediately before vTaskSuspend(NULL)

static TaskHandle_t s_refresh_task = NULL;
static EventGroupHandle_t s_refresh_events = NULL;
static volatile bool s_task_running = false;
static volatile bool s_sntp_synced_observed = false;
static volatile bool s_sntp_cache_touched = false;
static bool s_pico8_was_active = false;
static time_t s_last_full_refresh_complete = 0;
static uint32_t s_next_refresh_delay = REFRESH_INTERVAL_SECONDS;

// Arms the pin_lists GC kick fired at the next idle settle. Starts armed so
// the first settle after boot reclaims tombstones from a power loss; re-armed
// on playset switch and whenever a refresh actually completes. Without the
// flag the idle branch would kick the GC (an SD directory scan) on every
// re-entry while channels sit gated on a cooldown or missing connectivity.
static bool s_gc_kick_armed = true;

// PSRAM-backed stack for refresh task
static StackType_t *s_refresh_stack = NULL;
static StaticTask_t s_refresh_task_buffer;

// Channel IDs of refreshes that completed in the current iteration; reaped
// outside the scheduler mutex (reap blocks ~20 ms for FreeRTOS idle to drain
// the deleted task). Single-instance because only the ps_refresh task touches
// this; sized to PS_MAX_CHANNELS so a full sweep can collect them all.
static char s_completed_ids[PS_MAX_CHANNELS][64];
static size_t s_completed_count = 0;

/**
 * @brief Per-type freshness interval
 *
 * @return Interval in seconds, or 0 for types with no freshness interval
 *         (SDCARD/ARTWORK/PINNED). SDCARD and ARTWORK refreshes are local
 *         and cheap, so they are never "fresh" — the periodic cycle is the
 *         SD card's only periodic rescan path and ARTWORK's retry path for
 *         evicted/failed downloads.
 */
static uint32_t refresh_interval_for_type(ps_channel_type_t type)
{
    if (type == PS_CHANNEL_TYPE_GIPHY) {
        return config_store_get_giphy_refresh_interval();
    }
    if (type == PS_CHANNEL_TYPE_INSTITUTION) {
        return config_store_get_ai_refresh_sec();
    }
    if (type == PS_CHANNEL_TYPE_NAMED ||
        type == PS_CHANNEL_TYPE_USER ||
        type == PS_CHANNEL_TYPE_HASHTAG ||
        type == PS_CHANNEL_TYPE_REACTIONS) {
        return config_store_get_refresh_interval_sec();
    }
    return 0;
}

/**
 * @brief Whether a channel is provably inside its freshness window
 *
 * True only when every precondition for a trustworthy verdict holds: SNTP
 * has synchronized (pre-SNTP, time(NULL) is boot-relative garbage), the
 * channel has a real prior refresh stamp, its cache has entries, its type
 * carries a freshness interval, no force-refresh override is active, and
 * the interval has not yet elapsed. Anything uncertain returns false —
 * "not fresh" never blocks a refresh by itself; it only hands the channel
 * to the connectivity/cooldown gates and per-type dispatchers, which stay
 * the single source of truth for whether a refresh can actually run.
 *
 * Shared by the eligibility pre-emption gate (drop a queued fresh channel)
 * and the blanket re-queue sites (don't queue a fresh channel at all) so
 * the two predicates cannot drift: a channel skipped at re-queue is exactly
 * one the eligibility gate would have dropped.
 *
 * Reads the in-memory last_refresh mirror (hydrated from the sidecar at
 * playset load), not the on-disk sidecar — same source the pre-emption
 * gate has always used; see the defense-in-depth note in
 * refresh_channel_is_eligible for why dispatchers re-check the sidecar.
 */
static bool refresh_channel_is_fresh(const ps_channel_state_t *ch)
{
    if (!sntp_sync_is_synchronized()) return false;
    if (ch->last_refresh <= 0) return false;
    if (config_store_get_refresh_allow_override()) return false;

    uint32_t interval = refresh_interval_for_type(ch->type);
    if (interval == 0) return false;

    if (ch->cache == NULL || ch->cache->entry_count == 0) return false;

    time_t now = time(NULL);
    return now > 0 && (now - ch->last_refresh) < (time_t)interval;
}

/**
 * @brief Seconds until this channel could next need AND accept a refresh
 *
 * A channel is actionable when the latest of its constraints clears:
 *  - freshness expiry (last_refresh + interval; 0 when overdue/never refreshed)
 *  - rate-limit cooldown (Giphy key-wide, museum per-museum)
 *  - per-channel failure backoff (retry_at_us)
 *
 * "Due" without "actionable" is what used to clamp the periodic delay to its
 * 10 s floor: an overdue channel behind a 429 cooldown reported a stale time
 * in the past, so the idle machinery re-armed every 10 seconds for the whole
 * cooldown despite having provably nothing to do.
 *
 * Channels gated on connectivity (Wi-Fi for Giphy/museums, MQTT for Makapix)
 * have no timed bound — they return NEXT_CHECK_NO_DEADLINE and rely on the
 * connectivity signals (ps_refresh_signal_work on Wi-Fi/MQTT connect) plus
 * the REFRESH_INTERVAL_SECONDS fallback. SDCARD/ARTWORK carry no freshness
 * interval and ride along with whatever cycle fires next; only an active
 * failure backoff gives them a deadline of their own.
 *
 * Cooldowns and backoff are esp_timer-based (monotonic); freshness is wall
 * clock. All terms are combined as deltas from now, never as absolute times,
 * so the two clock domains never mix.
 *
 * @param ch       Channel state (caller must hold mutex)
 * @param now      Current wall-clock time
 * @param wifi_up  Cached p3a_state_has_wifi()
 * @param mqtt_up  Cached makapix_mqtt_is_connected()
 * @param out_gate Set to a short human-readable name of the binding
 *                 constraint (NULL when the channel has no deadline and no
 *                 connectivity gate, e.g. PINNED)
 * @return Seconds until next actionable, or NEXT_CHECK_NO_DEADLINE
 */
static uint32_t channel_next_actionable_sec(const ps_channel_state_t *ch, time_t now,
                                            bool wifi_up, bool mqtt_up,
                                            const char **out_gate)
{
    *out_gate = NULL;

    // Pinned channels are local-only and never refresh.
    if (ch->type == PS_CHANNEL_TYPE_PINNED) {
        return NEXT_CHECK_NO_DEADLINE;
    }

    // Failure backoff applies to every refreshable type.
    uint32_t backoff_in = 0;
    if (ch->retry_at_us > 0) {
        int64_t now_us = esp_timer_get_time();
        if (now_us < ch->retry_at_us) {
            backoff_in = (uint32_t)((ch->retry_at_us - now_us + 999999) / 1000000);
        }
    }

    // Freshness expiry: 0 when overdue or never refreshed.
    uint32_t stale_in = 0;
    uint32_t interval = refresh_interval_for_type(ch->type);
    if (interval > 0 && ch->last_refresh > 0 && now > 0) {
        time_t stale_at = ch->last_refresh + (time_t)interval;
        if (stale_at > now) stale_in = (uint32_t)(stale_at - now);
    }

    // SDCARD/ARTWORK: no freshness interval (see refresh_interval_for_type) —
    // contributing stale_in == 0 here would pin every cycle at the 10 s floor.
    if (interval == 0) {
        if (backoff_in > 0) {
            *out_gate = "failure backoff";
            return backoff_in;
        }
        return NEXT_CHECK_NO_DEADLINE;
    }

    // Connectivity gates and rate-limit cooldowns, by type.
    uint32_t gate_in = 0;
    const char *gate_name = NULL;
    if (ch->type == PS_CHANNEL_TYPE_GIPHY) {
        if (!wifi_up) {
            *out_gate = "Wi-Fi connectivity";
            return NEXT_CHECK_NO_DEADLINE;
        }
        gate_in = giphy_cooldown_remaining_sec();
        gate_name = "Giphy rate-limit cooldown";
        // Invalid-key latch: a key save clears it early via
        // ps_refresh_signal_work; this deadline only times the reprobe.
        uint32_t auth_in = giphy_auth_invalid_remaining_sec();
        if (auth_in > gate_in) {
            gate_in = auth_in;
            gate_name = "invalid Giphy API key";
        }
        // Missing-key parking: same shape, config-read instead of reprobe.
        uint32_t nokey_in = giphy_no_key_remaining_sec();
        if (nokey_in > gate_in) {
            gate_in = nokey_in;
            gate_name = "no Giphy API key";
        }
    } else if (ch->type == PS_CHANNEL_TYPE_INSTITUTION) {
        if (!wifi_up) {
            *out_gate = "Wi-Fi connectivity";
            return NEXT_CHECK_NO_DEADLINE;
        }
        char museum_id[16] = {0};
        char axis[32] = {0};
        if (art_institution_parse_spec(ch->spec_name, museum_id, sizeof(museum_id),
                                       axis, sizeof(axis)) == ESP_OK) {
            gate_in = art_institution_rate_limit_remaining(museum_id);
        }
        gate_name = "museum rate-limit cooldown";
    } else {
        // Makapix types are MQTT-gated, except the promoted channel which
        // falls back to HTTPS over plain Wi-Fi (mirrors the eligibility gate).
        bool promoted_https = (ch->type == PS_CHANNEL_TYPE_NAMED &&
                               strcmp(ch->spec_name, "promoted") == 0 && wifi_up);
        if (!mqtt_up && !promoted_https) {
            *out_gate = "Makapix connectivity";
            return NEXT_CHECK_NO_DEADLINE;
        }
    }

    uint32_t next_in = stale_in;
    *out_gate = "channel freshness";
    if (gate_in > next_in) {
        next_in = gate_in;
        *out_gate = gate_name;
    }
    if (backoff_in > next_in) {
        next_in = backoff_in;
        *out_gate = "failure backoff";
    }
    return next_in;
}

/**
 * @brief Decide whether a channel is eligible to refresh right now
 *
 * Applies all type-specific gates: Wi-Fi for Giphy (plus 429 cooldown), MQTT
 * for non-promoted Makapix channels, etc. Side effects: when Makapix is
 * permanently unavailable, clears refresh_pending so the channel doesn't sit
 * "refreshing" forever in the UI; when the channel is still within its
 * freshness interval, also clears refresh_pending so it falls out of the
 * queue without ever showing as "refreshing" or being briefly picked just
 * to be no-op'd by the dispatcher's freshness gate.
 *
 * @return true if the channel should be considered by the picker
 */
static bool refresh_channel_is_eligible(ps_channel_state_t *ch, bool mqtt_ready)
{
    if (!ch->refresh_pending || ch->refresh_in_progress) {
        return false;
    }

    // Pinned channels are fully loaded from local NVS at playset-load time and
    // have no remote source. Drop refresh_pending so they don't sit forever in
    // the queue (especially relevant when MQTT is down, which would otherwise
    // gate them at the Makapix branch below).
    if (ch->type == PS_CHANNEL_TYPE_PINNED) {
        ch->refresh_pending = false;
        return false;
    }

    // Pre-empt the per-dispatcher freshness gate. Without this, every channel
    // queued unconditionally (the playset executor queues all channels at
    // load) sits at refreshing=true until the picker rotates through and the
    // dispatcher's gate skips it as "still fresh". While the Giphy 429
    // cooldown is active those channels can't even be picked, so they'd
    // appear queued indefinitely despite having no real work to do.
    //
    // Mirrors the per-type gates in the dispatcher (Giphy: ~line 730,
    // Makapix: ~line 825). Those gates remain as defense-in-depth — they
    // read the on-disk sidecar, which is the canonical source and may
    // legitimately disagree with the in-memory mirror after a partial Giphy
    // refresh (in-memory bumped, disk save skipped on incomplete fetch).
    // SDCARD/ARTWORK have no dispatcher-side freshness gate, so we don't
    // apply one here either — their refreshes are local and cheap.
    if (refresh_channel_is_fresh(ch)) {
        ch->refresh_pending = false;
        ESP_LOGI(TAG, "Channel '%s' still fresh (last refresh %lds ago, interval %lus), dropped from queue",
                 ch->display_name,
                 (long)(time(NULL) - ch->last_refresh),
                 (unsigned long)refresh_interval_for_type(ch->type));
        return false;
    }

    // Failure backoff: a channel that just failed waits out its per-channel
    // exponential backoff before re-dispatch. Pending stays set (mirrors the
    // rate-limit cooldown pattern below) so the retry fires within ~2 s of
    // the backoff expiring via the periodic eligibility re-check. Checked
    // before the ARTWORK early-accept so failed artwork downloads back off
    // too instead of re-downloading every cycle.
    //
    // The user's force-refresh override bypasses the backoff — it's an
    // explicit "I fixed it, try again" signal and a transient failure may
    // well be fixed. The 429 cooldowns below deliberately do NOT honor the
    // override: a rate-limited key will fail again no matter what.
    if (ch->retry_at_us > 0 &&
        !config_store_get_refresh_allow_override() &&
        esp_timer_get_time() < ch->retry_at_us) {
        return false;
    }

    // Artwork channels don't need MQTT — they download directly
    if (ch->type == PS_CHANNEL_TYPE_ARTWORK) {
        return true;
    }

    // Giphy channels need Wi-Fi but not MQTT
    if (ch->type == PS_CHANNEL_TYPE_GIPHY) {
        if (!p3a_state_has_wifi()) return false;
        // A 429 from Giphy applies to the API key, not one channel — while
        // the cooldown is active, leave Giphy channels pending so they
        // retry naturally once the quota window resets.
        if (giphy_is_rate_limited()) return false;
        // A 401/403 likewise applies to the key: while the auth-invalid
        // latch is engaged, leave channels pending until a key save clears
        // it or the hourly reprobe window opens. Unlike the 429 cooldown,
        // the force-refresh override DOES bypass this gate — the key may
        // have been fixed at Giphy's end without its value changing, and a
        // retry can then succeed.
        if (giphy_is_auth_invalid() &&
            !config_store_get_refresh_allow_override()) return false;
        // Missing key: same parking, no network involved. The override
        // bypass doubles as a manual re-read of the config for key writes
        // that bypassed PUT /config.
        if (giphy_is_no_key() &&
            !config_store_get_refresh_allow_override()) return false;
        return true;
    }

    // Institution (museum) channels need Wi-Fi but not MQTT. Cooldown is
    // per-museum (AIC's 60-req/min is the binding constraint); while a
    // museum is rate-limited, leave its channels pending so they retry
    // once the cooldown clears. ESP_LOGW happens at dispatch site (see the
    // INSTITUTION arm below) so this gate stays silent.
    if (ch->type == PS_CHANNEL_TYPE_INSTITUTION) {
        if (!p3a_state_has_wifi()) return false;
        char museum_id[16] = {0};
        char axis[32] = {0};
        if (art_institution_parse_spec(ch->spec_name, museum_id, sizeof(museum_id),
                                       axis, sizeof(axis)) == ESP_OK) {
            if (art_institution_is_rate_limited(museum_id)) return false;
        }
        return true;
    }

    // Promoted named channel can refresh via HTTPS without MQTT
    if (ch->type == PS_CHANNEL_TYPE_NAMED &&
        strcmp(ch->spec_name, "promoted") == 0 &&
        p3a_state_has_wifi()) {
        return true;
    }

    // For Makapix channels, only proceed if MQTT is connected. By elimination,
    // every type that reaches here is a non-open Makapix channel — the same set
    // channel_requires_makapix() classifies as needing registration.
    if (ch->type != PS_CHANNEL_TYPE_SDCARD && !mqtt_ready) {
        // If Makapix is permanently unavailable (no registration or invalid
        // credentials), clear refresh_pending so the channel doesn't stay
        // stuck in "refreshing" state in the UI forever.
        makapix_state_t mstate = makapix_get_state();
        if (mstate == MAKAPIX_STATE_IDLE ||
            mstate == MAKAPIX_STATE_REGISTRATION_INVALID) {
            ch->refresh_pending = false;
            ESP_LOGI(TAG, "Channel '%s' refresh skipped (no Makapix connection)",
                     ch->display_name);
        }
        return false;
    }

    return true;
}

// True iff this channel can only be refreshed through a registered Makapix
// MQTT connection (i.e. NOT openable without registration). This is the
// classification mirror of the per-type handling in
// refresh_channel_is_eligible() above: every type that returns early there
// before the Makapix MQTT gate (line ~425) is "open". Enumerated explicitly
// (no default:true) so a future channel type can't false-trigger the banner.
static bool channel_requires_makapix(ps_channel_type_t type, const char *name)
{
    switch (type) {
        case PS_CHANNEL_TYPE_NAMED:
            return strcmp(name, "promoted") != 0;  // promoted is open via HTTPS
        case PS_CHANNEL_TYPE_USER:
        case PS_CHANNEL_TYPE_HASHTAG:
        case PS_CHANNEL_TYPE_REACTIONS:
            return true;   // Makapix-sourced, need a registered MQTT connection
        case PS_CHANNEL_TYPE_SDCARD:       // local files
        case PS_CHANNEL_TYPE_ARTWORK:      // direct download, no MQTT
        case PS_CHANNEL_TYPE_GIPHY:        // Giphy
        case PS_CHANNEL_TYPE_INSTITUTION:  // museums over IIIF
        case PS_CHANNEL_TYPE_PINNED:       // loaded from local NVS, no remote source
        default:
            return false;
    }
}

bool play_scheduler_playset_needs_registration(const ps_playset_t *playset)
{
    if (!playset) return false;
    // Only the two "permanently unavailable" states the refresh dispatcher
    // itself acts on (refresh_channel_is_eligible, lines ~429-431) mean these
    // channels can never refresh. Transient CONNECTING/DISCONNECTED states are
    // recoverable, so they raise no banner.
    makapix_state_t st = makapix_get_state();
    if (st != MAKAPIX_STATE_IDLE && st != MAKAPIX_STATE_REGISTRATION_INVALID) {
        return false;
    }
    for (size_t i = 0; i < playset->channel_count; i++) {
        if (channel_requires_makapix(playset->channels[i].type,
                                     playset->channels[i].name)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Find next channel that needs refresh
 *
 * Among all eligible channels (see refresh_channel_is_eligible), returns the
 * one with the smallest last_refresh timestamp — i.e., the channel that has
 * gone the longest without a successful refresh. Insertion order breaks ties
 * (channels at the same last_refresh, including all-zero on first boot, are
 * picked left-to-right).
 *
 * Oldest-first matters when the API quota or link bandwidth can't service
 * every channel in a cycle (e.g., Giphy keys with a 429 cap): the staler
 * channels get the slot before fresher ones do.
 *
 * @param state Scheduler state (caller must hold mutex)
 * @return Channel index, or -1 if none eligible
 */
static int find_next_pending_refresh(ps_state_t *state)
{
    bool mqtt_ready = makapix_mqtt_is_connected();

    int best_idx = -1;
    time_t best_last_refresh = 0;

    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        if (!refresh_channel_is_eligible(ch, mqtt_ready)) {
            continue;
        }

        if (best_idx < 0 || ch->last_refresh < best_last_refresh) {
            best_idx = (int)i;
            best_last_refresh = ch->last_refresh;
        }
    }

    return best_idx;
}

// External: check if download manager is actively downloading
extern bool download_manager_is_busy(void);
// External: check if an animation is on screen. Progress callbacks below
// re-check this dynamically: playback can begin mid-refresh (LAi
// first-available trigger), and a progress push landing after the
// swap-complete clear would re-cover the playing artwork until the NEXT
// swap — there is no other path that clears the message slot.
extern bool animation_player_is_animation_ready(void);

/**
 * @brief Progress callback for Giphy channel refresh
 *
 * Updates the UI with page-fetch progress during fresh start.
 * Goes silent once an animation is on screen (same dynamic re-check as
 * dl_progress_cb in download_manager.c — playback may start mid-refresh),
 * and yields to download progress when the download manager is busy.
 */
static void giphy_refresh_ui_cb(int offset, int cache_size, void *ctx)
{
    // Playback may have started mid-refresh — never cover a live animation
    // with index-fetch progress.
    if (animation_player_is_animation_ready()) return;

    // Don't override download progress
    if (download_manager_is_busy()) return;

    const char *name = (const char *)ctx;
    // The fetch loop reports whole pages and can overshoot the cache cap
    // (e.g. 150 fetched against a 128-entry cap) — clamp so the user never
    // sees "150/128" / >100%.
    int shown = (cache_size > 0 && offset > cache_size) ? cache_size : offset;
    int pct = (cache_size > 0) ? (shown * 100) / cache_size : -1;
    char detail[64];
    snprintf(detail, sizeof(detail), "Fetching artwork list (%d/%d)", shown, cache_size);
    p3a_render_set_channel_message(name, P3A_CHANNEL_MSG_LOADING, pct, detail);
}

/**
 * @brief Progress callback for artwork downloads
 *
 * Updates the UI with download progress percentage.
 */
static void artwork_download_progress_cb(size_t bytes_read, size_t content_length, void *user_ctx)
{
    (void)user_ctx;  // Unused

    // Playback may have started while this download was in flight (periodic
    // refresh of a mixed playset) — don't cover a live animation with
    // progress for a background fetch.
    if (animation_player_is_animation_ready()) return;

    int percent = 0;
    if (content_length > 0) {
        percent = (int)((bytes_read * 100) / content_length);
        if (percent > 100) percent = 100;
    }

    p3a_render_set_channel_message("Artwork", P3A_CHANNEL_MSG_DOWNLOADING, percent, NULL);
}

/**
 * @brief Refresh an artwork channel (download if needed)
 *
 * Checks if file already exists, otherwise downloads it.
 * Sets ch->active = true when file is ready for playback.
 */
static esp_err_t refresh_artwork_channel(ps_channel_state_t *ch)
{
    // artwork_state is a single ps_state-level member shared by the one active
    // ARTWORK channel (always channels[0]); ch->active stays per-channel.
    ps_artwork_state_t *art = &ps_get_state()->artwork_state;

    ESP_LOGI(TAG, "Refreshing artwork channel: %s", art->storage_key);

    // Check if file already exists
    struct stat st;
    if (stat(art->filepath, &st) == 0 && st.st_size > 0) {
        ESP_LOGI(TAG, "Artwork already in vault: %s", art->filepath);
        ch->active = true;
        art->download_pending = false;
        return ESP_OK;
    }

    // Need to download - check if we have URL
    if (art->art_url[0] == '\0') {
        // Local file that doesn't exist
        ESP_LOGE(TAG, "Artwork file not found and no URL to download: %s", art->filepath);
        ch->active = false;
        art->download_pending = false;
        return ESP_ERR_NOT_FOUND;
    }

    // Download the artwork
    art->download_in_progress = true;
    if (!animation_player_is_animation_ready()) {
        p3a_render_set_channel_message("Artwork", P3A_CHANNEL_MSG_DOWNLOADING, 0, NULL);
    }

    ESP_LOGI(TAG, "Downloading artwork: %s", art->art_url);

    char downloaded_path[256] = {0};
    esp_err_t err = makapix_artwork_download_with_progress(
        art->art_url,
        art->storage_key,
        downloaded_path, sizeof(downloaded_path),
        artwork_download_progress_cb, NULL
    );

    art->download_in_progress = false;

    if (err == ESP_OK) {
        // Update filepath to actual downloaded path (in case it differs)
        strlcpy(art->filepath, downloaded_path, sizeof(art->filepath));
        ch->active = true;
        art->download_pending = false;
        // Don't clear the message here - the animation player will clear it
        // after the buffer swap completes for seamless transition (no flash)
        ESP_LOGI(TAG, "Artwork download complete: %s", downloaded_path);
    } else {
        ESP_LOGE(TAG, "Artwork download failed: %s", esp_err_to_name(err));
        p3a_render_set_channel_message("Artwork", P3A_CHANNEL_MSG_ERROR, -1,
                                        esp_err_to_name(err));
        // Keep error message visible briefly
        vTaskDelay(pdMS_TO_TICKS(3000));
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
        ch->active = false;
        art->download_pending = false;
    }

    return err;
}

/**
 * @brief Refresh an SD card channel
 *
 * Rebuilds the sdcard.bin index by scanning the animations folder.
 */
static esp_err_t refresh_sdcard_channel(ps_channel_state_t *ch)
{
    ESP_LOGI(TAG, "Refreshing SD card channel");

    esp_err_t err = ps_build_sdcard_index();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build SD card index: %s", esp_err_to_name(err));
        return err;
    }

    // Load the cache file into memory (160-byte sdcard_index_entry_t format)
    esp_err_t load_err = ps_load_channel_cache(ch);
    if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load SD card cache: %s", esp_err_to_name(load_err));
    }

    return ESP_OK;
}

/**
 * @brief Refresh a Makapix channel
 *
 * Uses the dedicated makapix_refresh_channel_index() API to trigger
 * background refresh without channel switching or navigation.
 */
static esp_err_t refresh_makapix_channel(ps_channel_state_t *ch)
{
    ESP_LOGI(TAG, "Refreshing Makapix channel: %s (type=%d)", ch->display_name, ch->type);

    // Use type and spec fields directly — no channel_id string parsing needed
    esp_err_t err = ESP_OK;
    if (ch->type == PS_CHANNEL_TYPE_NAMED) {
        err = makapix_refresh_channel_index(ch->spec_name, NULL);
    } else if (ch->type == PS_CHANNEL_TYPE_USER) {
        err = makapix_refresh_channel_index("by_user", ch->identifier);
    } else if (ch->type == PS_CHANNEL_TYPE_REACTIONS) {
        err = makapix_refresh_channel_index("reactions", ch->identifier);
    } else if (ch->type == PS_CHANNEL_TYPE_HASHTAG) {
        err = makapix_refresh_channel_index("hashtag", ch->identifier);
    } else {
        ESP_LOGW(TAG, "Unknown Makapix channel type: %d", ch->type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        // MQTT not connected - return this error so caller can queue for retry
        ESP_LOGD(TAG, "MQTT not connected, will retry when connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to trigger Makapix refresh: %s", esp_err_to_name(err));
        return err;
    }

    // The actual refresh happens asynchronously in Makapix
    // Mark channel as waiting for async completion
    ch->refresh_async_pending = true;

    // Only load cache if not already loaded (avoid double-load during channel switch)
    // When switching channels, ps_load_channel_cache is called first, then refresh is triggered.
    // Reloading here would free the cache that was just loaded, causing heap corruption.
    if (!ch->cache_loaded) {
        // Optimistically load any existing cache (may have stale data)
        esp_err_t cache_err = ps_load_channel_cache(ch);
        if (cache_err != ESP_OK && cache_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGD(TAG, "No existing cache for '%s': %s", ch->display_name, esp_err_to_name(cache_err));
        }
    } else {
        ESP_LOGD(TAG, "Cache already loaded for '%s', skipping reload", ch->display_name);
    }

    // Return special code to indicate async in progress
    // The caller should NOT report completion until async finishes
    return ESP_ERR_NOT_FINISHED;
}

/**
 * @brief Count channels with refresh currently in progress
 *
 * @param state Scheduler state (caller must hold mutex)
 * @return Number of channels with refresh_in_progress set
 */
static int count_in_flight_refreshes(ps_state_t *state)
{
    int count = 0;
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].refresh_in_progress) {
            count++;
        }
    }
    return count;
}

/**
 * @brief Background refresh task
 *
 * Runs continuously, processing pending channel refreshes up to
 * REFRESH_MAX_CONCURRENT at a time.
 */
static void refresh_task(void *arg)
{
    ps_state_t *state = ps_get_state();

    ESP_LOGI(TAG, "Refresh task started");
    s_task_running = true;

    while (s_task_running) {
        // Wait for work or shutdown
        EventBits_t bits = xEventGroupWaitBits(
            s_refresh_events,
            REFRESH_EVENT_WORK_AVAILABLE | REFRESH_EVENT_SHUTDOWN,
            pdTRUE,  // Clear bits on exit
            pdFALSE, // Wait for any bit
            pdMS_TO_TICKS(REFRESH_CHECK_INTERVAL_MS)
        );

        if (bits & REFRESH_EVENT_SHUTDOWN) {
            ESP_LOGI(TAG, "Shutdown requested");
            break;
        }

        // One-shot: when SNTP first synchronizes, re-queue Giphy channels.
        // A pre-SNTP refresh may have bypassed cooldown (check-side guard) and
        // skipped the metadata save (save-side guard). Re-queuing lets the
        // cooldown logic re-evaluate with a valid clock and the preserved
        // on-disk last_refresh from the previous session.
        // Once per boot: as soon as SNTP syncs, touch all active channel cache
        // files. Channels loaded before SNTP sync had their touch skipped (the
        // clock was unusable), so this retroactively marks them as recently used.
        if (!s_sntp_cache_touched && sntp_sync_is_synchronized()) {
            s_sntp_cache_touched = true;
            xSemaphoreTake(state->mutex, portMAX_DELAY);
            for (size_t i = 0; i < state->channel_count; i++) {
                ps_touch_cache_file(state->channels[i].channel_id, state->channels[i].type);
            }
            xSemaphoreGive(state->mutex);
        }

        // Once per boot: when SNTP first synchronizes, re-queue channels.
        // A pre-SNTP refresh may have bypassed cooldown (check-side guard) and
        // skipped the metadata save (save-side guard). Re-queuing lets the
        // cooldown logic re-evaluate with a valid clock and the preserved
        // on-disk last_refresh from the previous session.
        //
        // Selective: this runs after sync, so refresh_channel_is_fresh()
        // gives a trustworthy verdict against the sidecar-hydrated
        // last_refresh — channels provably inside their window are left
        // alone instead of being queued just to be dropped by the
        // eligibility gate. A channel that DID refresh pre-SNTP has no
        // valid stamp (both the in-memory bump and the sidecar save are
        // SNTP-gated), so it reads as not-fresh and is re-queued for the
        // valid-clock re-evaluation this one-shot exists for.
        if (!s_sntp_synced_observed && sntp_sync_is_synchronized()) {
            s_sntp_synced_observed = true;
            // Reset periodic timer so the SNTP-triggered round recalculates
            // the adaptive delay with valid timestamps.
            s_last_full_refresh_complete = 0;
            ESP_LOGI(TAG, "SNTP synchronized - re-evaluating channel freshness");
            xSemaphoreTake(state->mutex, portMAX_DELAY);
            for (size_t i = 0; i < state->channel_count; i++) {
                ps_channel_type_t t = state->channels[i].type;
                if ((t == PS_CHANNEL_TYPE_GIPHY ||
                     t == PS_CHANNEL_TYPE_NAMED ||
                     t == PS_CHANNEL_TYPE_USER ||
                     t == PS_CHANNEL_TYPE_REACTIONS ||
                     t == PS_CHANNEL_TYPE_HASHTAG ||
                     t == PS_CHANNEL_TYPE_INSTITUTION) &&
                    !state->channels[i].refresh_in_progress &&
                    !state->channels[i].refresh_pending &&
                    !refresh_channel_is_fresh(&state->channels[i])) {
                    state->channels[i].refresh_pending = true;
                }
            }
            xSemaphoreGive(state->mutex);
        }

        // One-shot: when PICO-8 mode ends, re-queue Makapix channels for refresh.
        // The refresh tasks exit after one cycle, so after PICO-8 paused them
        // the Play Scheduler must re-trigger them.
        //
        // Selective: a channel that completed its refresh before PICO-8 took
        // over has a valid stamp and is provably fresh — skip it. A refresh
        // that PICO-8 interrupted never bumped last_refresh, so the channel
        // reads as not-fresh and is re-queued, which is the case this
        // one-shot exists for.
        {
            bool pico8_active = (p3a_state_get() == P3A_STATE_PICO8_STREAMING);
            if (s_pico8_was_active && !pico8_active) {
                ESP_LOGI(TAG, "PICO-8 mode ended - re-evaluating channel freshness");
                s_last_full_refresh_complete = 0;
                xSemaphoreTake(state->mutex, portMAX_DELAY);
                for (size_t i = 0; i < state->channel_count; i++) {
                    ps_channel_type_t t = state->channels[i].type;
                    if ((t == PS_CHANNEL_TYPE_USER ||
                         t == PS_CHANNEL_TYPE_REACTIONS ||
                         t == PS_CHANNEL_TYPE_HASHTAG ||
                         t == PS_CHANNEL_TYPE_NAMED ||
                         t == PS_CHANNEL_TYPE_INSTITUTION) &&
                        !state->channels[i].refresh_in_progress &&
                        !state->channels[i].refresh_pending &&
                        !refresh_channel_is_fresh(&state->channels[i])) {
                        state->channels[i].refresh_pending = true;
                    }
                }
                xSemaphoreGive(state->mutex);
            }
            s_pico8_was_active = pico8_active;
        }

        // Check for async Makapix refresh completions (non-blocking poll)
        if (makapix_channel_wait_for_ps_refresh_done(0)) {
            bool should_trigger = false;
            s_completed_count = 0;
            xSemaphoreTake(state->mutex, portMAX_DELAY);
            for (size_t i = 0; i < state->channel_count; i++) {
                ps_channel_state_t *ch = &state->channels[i];
                bool refresh_ok = false;
                if (ch->refresh_async_pending &&
                    makapix_ps_refresh_check_and_clear(ch->channel_id, &refresh_ok)) {
                    // This channel's async refresh completed
                    ch->refresh_async_pending = false;
                    ch->refresh_in_progress = false;

                    if (refresh_ok) {
                        // The walk ran to its end: clear the failure backoff
                        // and arm the pin_lists GC kick for the next idle
                        // settle.
                        ch->fail_streak = 0;
                        ch->retry_at_us = 0;
                        s_gc_kick_armed = true;

                        // Mirror the makapix-side save: only stamp a real time when
                        // SNTP is synced (matches makapix_channel_refresh.c).
                        // The in-memory value may differ from the disk save by up
                        // to one polling interval (~2s); harmless for picker order.
                        if (sntp_sync_is_synchronized()) {
                            ch->last_refresh = time(NULL);
                        }
                    } else {
                        // Incomplete walk (cancelled, or transport/server error
                        // mid-pagination): the makapix side kept the merged
                        // entries but skipped eviction and the on-disk
                        // timestamp. Mirror that here — leave last_refresh
                        // stale so the freshness gate doesn't park a truncated
                        // channel for a full interval, and arm the generic
                        // per-channel backoff (same shape as the sync dispatch
                        // failure arm below).
                        uint32_t shift = ch->fail_streak < 5 ? ch->fail_streak : 5;
                        uint32_t backoff_sec = RETRY_BACKOFF_BASE_SEC << shift;
                        if (backoff_sec > RETRY_BACKOFF_CAP_SEC) backoff_sec = RETRY_BACKOFF_CAP_SEC;
                        uint32_t backoff_ms = backoff_sec * (800 + (esp_random() % 401));
                        ch->retry_at_us = esp_timer_get_time() + (int64_t)backoff_ms * 1000;
                        if (ch->fail_streak < UINT8_MAX) ch->fail_streak++;
                        ch->refresh_pending = true;
                        ESP_LOGW(TAG, "Channel '%s' async refresh incomplete, failure #%u, retry in ~%lus",
                                 ch->display_name, (unsigned)ch->fail_streak,
                                 (unsigned long)(backoff_ms / 1000));
                    }

                    // Queue this channel for eager reap after the mutex is
                    // released. Without reaping, every channel that finishes
                    // a refresh leaves a parked task behind holding ~12 KB of
                    // stack and a TCB until the next refresh on that channel.
                    if (s_completed_count < PS_MAX_CHANNELS) {
                        strlcpy(s_completed_ids[s_completed_count++], ch->channel_id,
                                sizeof(s_completed_ids[0]));
                    }

                    // Keep in-memory state - don't reload from disk.
                    // The cache was populated during batch merges and already has the correct state.
                    // Reloading would replace it with stale disk data (race with debounced save).
                    if (ch->cache) {
                        ch->cache_loaded = true;
                        ch->active = (ch->cache->available_count > 0);
                        ESP_LOGI(TAG, "Channel '%s': keeping in-memory cache (%zu entries, %zu available)",
                                 ch->display_name, ch->cache->entry_count, ch->cache->available_count);

                        // Mark dirty to ensure eventual persistence
                        if (!ch->cache->dirty) {
                            ch->cache->dirty = true;
                        }
                    } else {
                        ESP_LOGW(TAG, "Channel '%s': no in-memory cache after refresh", ch->display_name);
                    }

                    size_t entry_count = ch->cache ? ch->cache->entry_count : ch->entry_count;
                    ESP_LOGI(TAG, "Channel '%s' async refresh %s: %zu entries, active=%d",
                             ch->display_name, refresh_ok ? "complete" : "incomplete",
                             entry_count, ch->active);

                    // Recalculate weights
                    ps_swrr_calculate_weights(state);

                    // Signal that refresh is done - this wakes download task
                    // which may be waiting for initial refresh to complete
                    makapix_channel_signal_refresh_done();

                    // Reset download cursors so download manager rescans the new cache
                    extern void download_manager_reset_cursors(void);
                    download_manager_reset_cursors();

                    // Track if we should trigger playback (once, after the loop)
                    // Only trigger if channel is active (has locally-available artworks),
                    // not just index entries. Matches the sync path check at line 788.
                    if (ch->active && entry_count > 0 && !state->first_swap_emitted) {
                        should_trigger = true;
                    }
                }
            }

            xSemaphoreGive(state->mutex);

            // Reap each completed channel's parked refresh task to release
            // its TCB and 12 KB stack. Done outside the scheduler mutex
            // because reap blocks tens of ms waiting for FreeRTOS idle to
            // drain the deleted task's bookkeeping.
            for (size_t i = 0; i < s_completed_count; i++) {
                makapix_reap_finished_refresh(s_completed_ids[i]);
            }
            s_completed_count = 0;

            // Trigger playback once outside the loop and mutex
            if (should_trigger) {
                ESP_LOGI(TAG, "Async refresh complete - triggering playback");

                // Signal download manager to rescan for any missing files
                extern void download_manager_rescan(void);
                download_manager_rescan();

                // Don't clear the loading message here — let the animation player
                // clear it after the buffer swap completes for seamless transition.
                // (Same pattern as the sync path at lines 799-801.)
                if (play_scheduler_next(NULL) == ESP_OK) {
                    xSemaphoreTake(state->mutex, portMAX_DELAY);
                    state->first_swap_emitted = true;
                    ps_assert_first_swap_invariant(state, "async_refresh_complete");
                    xSemaphoreGive(state->mutex);
                }
            }
        }

        // Async-refresh timeout sweep — runs every iteration, not gated on a
        // signal arriving. If a refresh was cancelled or its slot was rebound
        // by a playset switch, no completion signal will ever match it; only
        // this elapsed-time check can rescue an orphaned refresh_async_pending.
        {
            uint32_t async_timeout = refresh_async_timeout_ms();
            xSemaphoreTake(state->mutex, portMAX_DELAY);
            for (size_t i = 0; i < state->channel_count; i++) {
                ps_channel_state_t *tch = &state->channels[i];
                if (tch->refresh_async_pending &&
                    (xTaskGetTickCount() - tch->refresh_start_tick) > pdMS_TO_TICKS(async_timeout)) {
                    ESP_LOGW(TAG, "Channel '%s' async refresh timed out after %lu ms",
                             tch->display_name, (unsigned long)async_timeout);
                    tch->refresh_async_pending = false;
                    tch->refresh_in_progress = false;
                    tch->refresh_pending = true;  // Re-queue for retry
                }
            }
            xSemaphoreGive(state->mutex);
        }

        // Concurrency gate: limit simultaneous refreshes
        xSemaphoreTake(state->mutex, portMAX_DELAY);
        if (count_in_flight_refreshes(state) >= REFRESH_MAX_CONCURRENT) {
            xSemaphoreGive(state->mutex);
            continue;  // Back to top — will poll async completions next iteration
        }

        // Find next pending refresh (already under mutex)
        int ch_idx = find_next_pending_refresh(state);

        if (ch_idx < 0) {
            // No channels pending — check if periodic refresh is due
            bool any_in_progress = false;
            for (size_t i = 0; i < state->channel_count; i++) {
                if (state->channels[i].refresh_async_pending || state->channels[i].refresh_in_progress) {
                    any_in_progress = true;
                    break;
                }
            }

            if (!any_in_progress && state->channel_count > 0) {
                time_t now = time(NULL);
                if (s_last_full_refresh_complete == 0) {
                    s_last_full_refresh_complete = now;
                    // Auto-reset refresh override only if Giphy isn't still
                    // gated behind a rate-limit cooldown — we don't want to
                    // discard the user's explicit "force refresh" intent for
                    // channels that haven't actually been refreshed yet.
                    if (!giphy_is_rate_limited() &&
                        config_store_get_refresh_allow_override()) {
                        config_store_set_refresh_allow_override(false);
                    }
                    // Gate-aware deadline sweep: each channel reports when it
                    // could next need AND accept a refresh — freshness expiry,
                    // rate-limit cooldown expiry, and failure backoff combined
                    // (whichever clears last; see channel_next_actionable_sec).
                    // Channels gated on connectivity report no deadline: the
                    // Wi-Fi/MQTT connect signals wake the task directly, and
                    // the REFRESH_INTERVAL_SECONDS fallback is belt-and-braces.
                    //
                    // Without the gate terms, overdue-but-gated channels (e.g.
                    // a whole playset of Giphy channels behind a 429 cooldown)
                    // reported a stale time in the past, clamping the delay to
                    // the 10 s floor — the periodic machinery then spun
                    // uselessly for the entire cooldown (~60 requeue/log
                    // cycles per 10 min cooldown, plus a pin_lists GC kick
                    // each time).
                    //
                    // The deadline only times the requeue/log cycle. Gated
                    // channels keep refresh_pending and are picked up by the
                    // eligibility re-check within ~2 s of their gate opening,
                    // regardless of this delay.
                    bool wifi_up = p3a_state_has_wifi();
                    bool mqtt_up = makapix_mqtt_is_connected();
                    uint32_t min_next_sec = NEXT_CHECK_NO_DEADLINE;
                    const char *pending_gate = NULL;  // Binding gate of a still-pending channel
                    size_t still_pending = 0;
                    for (size_t i = 0; i < state->channel_count; i++) {
                        ps_channel_state_t *cch = &state->channels[i];
                        const char *gate = NULL;
                        uint32_t next_sec = channel_next_actionable_sec(cch, now, wifi_up,
                                                                        mqtt_up, &gate);
                        if (next_sec < min_next_sec) {
                            min_next_sec = next_sec;
                        }
                        if (cch->refresh_pending) {
                            still_pending++;
                            if (gate) pending_gate = gate;
                        }
                    }

                    if (min_next_sec == NEXT_CHECK_NO_DEADLINE) {
                        s_next_refresh_delay = REFRESH_INTERVAL_SECONDS;
                    } else if (min_next_sec < REFRESH_MIN_DELAY_SECONDS) {
                        s_next_refresh_delay = REFRESH_MIN_DELAY_SECONDS;
                    } else if (min_next_sec > REFRESH_INTERVAL_SECONDS) {
                        s_next_refresh_delay = REFRESH_INTERVAL_SECONDS;
                    } else {
                        s_next_refresh_delay = min_next_sec;
                    }

                    // Distinguish "everything actually refreshed" from "no
                    // eligible work right now": channels stay pending while
                    // gated on Wi-Fi / MQTT / rate-limit cooldowns / failure
                    // backoff (typical at boot before connectivity), and the
                    // old unconditional copy claimed completion at t=2.6s on a
                    // cold boot with nothing refreshed. Logged once per idle
                    // detection (the stamp above keeps this branch from
                    // re-entering); the named gate is the binding constraint
                    // of one of the pending channels, so the log tells the
                    // truth about WHY the device is waiting and for how long.
                    if (still_pending > 0) {
                        ESP_LOGI(TAG, "No eligible channels to refresh (%zu pending on %s). Next check in %lu seconds.",
                                 still_pending,
                                 pending_gate ? pending_gate : "connectivity",
                                 (unsigned long)s_next_refresh_delay);
                    } else {
                        ESP_LOGI(TAG, "All channels refreshed. Next freshness check in %lu seconds.",
                                 (unsigned long)s_next_refresh_delay);
                    }
                    /* Settled moment: nudge the pin_lists GC so any tombstones
                       left over from a power loss during a previous delete get
                       reclaimed. Armed at boot, on playset switch, and on
                       completed refreshes — NOT kicked on every idle re-entry,
                       which would rescan the SD card each cycle while channels
                       sit gated on a cooldown or missing connectivity. */
                    if (s_gc_kick_armed) {
                        s_gc_kick_armed = false;
                        pin_lists_gc_kick();
                    }
                } else if (now - s_last_full_refresh_complete >= (time_t)s_next_refresh_delay) {
                    // Selective re-queue: skip channels provably inside their
                    // freshness window. They used to be queued unconditionally
                    // and immediately dropped by the eligibility gate — pure
                    // churn (two INFO lines per fresh channel per cycle and a
                    // transient "refreshing" blip in the status API). The
                    // predicate is shared with that gate, so a channel skipped
                    // here is exactly one the gate would have dropped.
                    //
                    // Also skip channels already queued or in flight: the
                    // re-stamp was an idempotent no-op, but counting them made
                    // the cycle log claim "30 due" every cycle while every one
                    // of them sat gated on a cooldown. `due` now counts actual
                    // not-pending -> pending transitions, so the INFO line
                    // only fires when this cycle adds real work. Gated
                    // channels (Wi-Fi down, Giphy/museum 429, failure backoff)
                    // keep refresh_pending across cycles and retry within ~2s
                    // of the gate opening — the eligibility gates stay the
                    // deciders; is_fresh knows nothing about connectivity.
                    // Unregistered Makapix (state IDLE/REG_INVALID) channels
                    // have pending cleared by the eligibility gate, so they
                    // transition back here each cycle and retry within an
                    // hour of registration, as before.
                    //
                    // SDCARD/ARTWORK have no interval (never fresh): they
                    // re-queue every cycle, preserving the hourly SD rescan
                    // and evicted/failed-artwork retry. PINNED is local-only
                    // with nothing to refresh — skipped explicitly rather
                    // than queued for the gate to silently clear.
                    size_t due = 0;
                    for (size_t i = 0; i < state->channel_count; i++) {
                        ps_channel_state_t *cch = &state->channels[i];
                        if (cch->type == PS_CHANNEL_TYPE_PINNED) continue;
                        if (cch->refresh_pending || cch->refresh_in_progress) continue;
                        if (refresh_channel_is_fresh(cch)) continue;
                        cch->refresh_pending = true;
                        due++;
                    }
                    if (due > 0) {
                        ESP_LOGI(TAG, "Periodic refresh cycle: %zu channel(s) due (%lu seconds elapsed)",
                                 due, (unsigned long)s_next_refresh_delay);
                    } else {
                        ESP_LOGD(TAG, "Periodic freshness check: no channels due (%lu seconds elapsed)",
                                 (unsigned long)s_next_refresh_delay);
                    }
                    s_last_full_refresh_complete = 0;
                    s_next_refresh_delay = REFRESH_INTERVAL_SECONDS;
                    xSemaphoreGive(state->mutex);
                    if (due > 0) {
                        ps_refresh_signal_work();
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
            }

            xSemaphoreGive(state->mutex);
            continue;
        }

        ps_channel_state_t *ch = &state->channels[ch_idx];
        ch->refresh_in_progress = true;
        ch->refresh_start_tick = xTaskGetTickCount();
        ch->refresh_pending = false;

        // Copy what we need before releasing mutex
        ps_channel_type_t type = ch->type;
        char channel_id[64];
        strlcpy(channel_id, ch->channel_id, sizeof(channel_id));
        char identifier[33];
        strlcpy(identifier, ch->identifier, sizeof(identifier));
        char spec_name[33];
        strlcpy(spec_name, ch->spec_name, sizeof(spec_name));
        char display_name[65];
        strlcpy(display_name, ch->display_name, sizeof(display_name));
        uint32_t channel_offset = ch->offset;
        bool cache_has_entries = (ch->cache != NULL && ch->cache->entry_count > 0);

        xSemaphoreGive(state->mutex);

        // Perform refresh (outside mutex to avoid blocking scheduler)
        esp_err_t err = ESP_OK;
        // did_refresh distinguishes paths that actually invoked a refresh
        // primitive from no-op success paths (freshness skip, SNTP-deferred,
        // missing API key). Only the former should bump ch->last_refresh —
        // otherwise the freshness gate would keep extending the window
        // without doing any work.
        bool did_refresh = false;

        if (type == PS_CHANNEL_TYPE_SDCARD) {
            did_refresh = true;
            err = refresh_sdcard_channel(ch);
        } else if (type == PS_CHANNEL_TYPE_PINNED) {
            // Pinned lists are local-only; nothing to refresh. Eligibility
            // normally filters these out; this arm is belt-and-braces so a
            // bypass can't fall through to the Makapix path and log
            // "Unknown Makapix channel type: 8".
            err = ESP_OK;
        } else if (type == PS_CHANNEL_TYPE_ARTWORK) {
            did_refresh = true;
            err = refresh_artwork_channel(ch);
        } else if (type == PS_CHANNEL_TYPE_GIPHY) {
            // Verify that a Giphy API key is configured before attempting refresh
            char giphy_key_check[128];
            config_store_get_giphy_api_key(giphy_key_check, sizeof(giphy_key_check));
            if (giphy_key_check[0] == '\0') {
                ESP_LOGW(TAG, "Giphy channel '%s' skipped: no API key configured", display_name);
                // Park key-wide (eligibility gates until a key save or the
                // hourly config re-read) and notify once per episode — the
                // same treatment as a rejected key, minus the network.
                giphy_set_no_key();
                if (giphy_no_key_take_notification()) {
                    char giphy_display_name[64];
                    ps_get_display_name_from_spec(type, spec_name, identifier, giphy_display_name, sizeof(giphy_display_name));
                    p3a_render_set_channel_message(giphy_display_name, P3A_CHANNEL_MSG_ERROR, -1,
                                                   "Cannot refresh channel: no\n"
                                                   "Giphy API key configured.\n"
                                                   "Add one in Settings.");
                }
                err = ESP_ERR_NOT_FOUND;
            } else {
            // Check if Giphy channel was refreshed recently enough.
            // If the cache is empty (e.g. deleted), always refresh so the
            // user has something to see, even if the interval hasn't elapsed.
            char ch_path[128];
            channel_metadata_t giphy_meta = {0};
            if (sd_path_get_channel(ch_path, sizeof(ch_path)) == ESP_OK) {
                channel_metadata_load(channel_id, ch_path, &giphy_meta);
            } else {
                ESP_LOGE(TAG, "Cannot resolve channel directory; treating '%s' as never refreshed",
                         display_name);
            }

            time_t now = time(NULL);
            uint32_t interval = config_store_get_giphy_refresh_interval();
            bool allow_override = config_store_get_refresh_allow_override();
            if (!allow_override &&
                cache_has_entries &&
                giphy_meta.last_refresh > 0 && now > 0 &&
                (now - giphy_meta.last_refresh) < (time_t)interval) {
                if (!sntp_sync_is_synchronized()) {
                    ESP_LOGI(TAG, "Giphy channel '%s' deferred (SNTP not synchronized)", display_name); // Channels are re-checked on SNTP sync
                } else {
                    uint32_t remaining = interval - (uint32_t)(now - giphy_meta.last_refresh);
                    ESP_LOGI(TAG, "Giphy channel '%s' still fresh (last refresh %lds ago, interval %lus, stale in %lus), skipping",
                            display_name, (long)(now - giphy_meta.last_refresh), (unsigned long)interval, (unsigned long)remaining);
                }
                err = ESP_OK;  // Treat as successful no-op
            } else {
                if (allow_override) {
                    ESP_LOGI(TAG, "Channel '%s' refresh override active, bypassing interval check",
                             display_name);
                } else if (!cache_has_entries && giphy_meta.last_refresh > 0) {
                    ESP_LOGI(TAG, "Giphy channel '%s' cache is empty, forcing refresh despite interval",
                             display_name);
                }

                // Show loading message while Giphy API is being called.
                // During boot restore this replaces the "Waiting for
                // network..." placeholder pushed by execute_playset before
                // Wi-Fi was up.
                char giphy_display_name[64];
                // Use the channel's resolved display name (honors a custom
                // playset display_name) so this push shares identity with the
                // executor/navigation messages — the render-side arbitration
                // then dedups same-channel updates instead of flickering.
                strlcpy(giphy_display_name, display_name, sizeof(giphy_display_name));
                did_refresh = true;
                if (!animation_player_is_animation_ready()) {
                    p3a_render_set_channel_message(giphy_display_name, P3A_CHANNEL_MSG_LOADING, -1,
                                                   "Loading channel...");
                    const char *query = identifier[0] != '\0' ? identifier : NULL;
                    err = giphy_refresh_channel_with_progress(channel_id, query, channel_offset,
                              giphy_refresh_ui_cb, giphy_display_name);
                } else {
                    const char *query = identifier[0] != '\0' ? identifier : NULL;
                    err = giphy_refresh_channel(channel_id, query, channel_offset);
                }
                if (err == ESP_ERR_NOT_ALLOWED) {
                    // Check if this is a cancellation (from giphy_cancel_refresh)
                    // vs an invalid API key (HTTP 401/403 from giphy_fetch_page)
                    if (giphy_is_refresh_cancelled()) {
                        ESP_LOGI(TAG, "Giphy channel '%s' refresh cancelled", display_name);
                    } else if (giphy_auth_take_notification()) {
                        // Once per failure episode (re-armed by a key save or
                        // a successful refresh), not per retry: the auth
                        // latch keeps refreshes parked, and re-pushing the
                        // same error on every reprobe would only re-cover
                        // live artwork until the next swap.
                        char giphy_display_name[64];
                        ps_get_display_name_from_spec(type, spec_name, identifier, giphy_display_name, sizeof(giphy_display_name));
                        p3a_render_set_channel_message(giphy_display_name, P3A_CHANNEL_MSG_ERROR, -1,
                                                       "Cannot refresh channel: Giphy\n"
                                                       "rejected the API key (error 401).\n"
                                                       "Update the key in Settings.");
                    }
                } else if (err != ESP_OK) {
                    char giphy_display_name[64];
                    ps_get_display_name_from_spec(type, spec_name, identifier, giphy_display_name, sizeof(giphy_display_name));
                    const char *detail = "Giphy refresh failed";
                    char rate_limit_buf[128];
                    if (err == ESP_ERR_INVALID_RESPONSE) {
                        uint32_t remaining_min = (giphy_cooldown_remaining_sec() + 59) / 60;
                        if (remaining_min == 0) remaining_min = 1;
                        // Renderer caps at 3 lines (see ugfx_ui.c MAX_LINES) — keep
                        // line breaks aligned with that limit.
                        snprintf(rate_limit_buf, sizeof(rate_limit_buf),
                                 "Cannot refresh channel: Giphy API key\n"
                                 "rate limit reached (error 429).\n"
                                 "Device will retry in %u min.",
                                 (unsigned)remaining_min);
                        detail = rate_limit_buf;
                    }
                    // Self-gate on live playback: a 429 can now arrive
                    // mid-pagination (after a partial fetch already merged
                    // playable entries) while an animation is on screen — never
                    // cover live artwork with a refresh error the device will
                    // retry on its own. When nothing is playing yet (cold start,
                    // 429 on the first page) the message still informs the user.
                    if (!animation_player_is_animation_ready()) {
                        p3a_render_set_channel_message(giphy_display_name, P3A_CHANNEL_MSG_ERROR, -1, detail);
                    }
                }
            }
            }
        } else if (type == PS_CHANNEL_TYPE_INSTITUTION) {
            // Parse museum id once for cooldown checks and log messages.
            char museum_id[16] = {0};
            char axis_unused[32] = {0};
            art_institution_parse_spec(spec_name, museum_id, sizeof(museum_id),
                                       axis_unused, sizeof(axis_unused));

            // Defense-in-depth cooldown check — refresh_channel_is_eligible
            // already filters out rate-limited museums, but the window can
            // close between eligibility and dispatch.
            if (art_institution_is_rate_limited(museum_id)) {
                uint32_t remaining = art_institution_rate_limit_remaining(museum_id);
                ESP_LOGW(TAG, "Skipping '%s': museum '%s' rate-limited (%us remaining)",
                         display_name, museum_id, (unsigned)remaining);
                err = ESP_OK;  // Treat as no-op; refresh_pending stays cleared
            } else {
                // Freshness gate (canonical: on-disk sidecar, see giphy block above)
                char ai_ch_path[128];
                channel_metadata_t ai_meta = {0};
                if (sd_path_get_channel(ai_ch_path, sizeof(ai_ch_path)) == ESP_OK) {
                    channel_metadata_load(channel_id, ai_ch_path, &ai_meta);
                } else {
                    ESP_LOGE(TAG, "Cannot resolve channel directory; treating '%s' as never refreshed",
                             display_name);
                }

                time_t now = time(NULL);
                uint32_t interval = config_store_get_ai_refresh_sec();
                bool ai_allow_override = config_store_get_refresh_allow_override();
                if (!ai_allow_override &&
                    cache_has_entries &&
                    ai_meta.last_refresh > 0 && now > 0 &&
                    (now - ai_meta.last_refresh) < (time_t)interval) {
                    if (!sntp_sync_is_synchronized()) {
                        ESP_LOGI(TAG, "Institution channel '%s' deferred (SNTP not synchronized)", display_name);
                    } else {
                        uint32_t remaining = interval - (uint32_t)(now - ai_meta.last_refresh);
                        ESP_LOGI(TAG, "Institution channel '%s' still fresh (last refresh %lds ago, interval %lus, stale in %lus), skipping",
                                 display_name, (long)(now - ai_meta.last_refresh), (unsigned long)interval, (unsigned long)remaining);
                    }
                    err = ESP_OK;  // Treat as successful no-op
                } else {
                    if (ai_allow_override) {
                        ESP_LOGI(TAG, "Channel '%s' refresh override active, bypassing interval check", display_name);
                    } else if (!cache_has_entries && ai_meta.last_refresh > 0) {
                        ESP_LOGI(TAG, "Institution channel '%s' cache is empty, forcing refresh despite interval", display_name);
                    }
                    did_refresh = true;
                    err = art_institution_refresh_by_spec(channel_id, spec_name, identifier, channel_offset);
                    if (err == ESP_ERR_INVALID_RESPONSE) {
                        // 429 — cooldown already engaged inside the adapter.
                        uint32_t remaining_sec = art_institution_rate_limit_remaining(museum_id);
                        char rate_limit_buf[160];
                        snprintf(rate_limit_buf, sizeof(rate_limit_buf),
                                 "Cannot refresh channel: %s API\n"
                                 "rate limit reached (error 429).\n"
                                 "Device will retry in %u sec.",
                                 museum_id[0] ? museum_id : "museum",
                                 (unsigned)remaining_sec);
                        p3a_render_set_channel_message(display_name, P3A_CHANNEL_MSG_ERROR, -1, rate_limit_buf);
                    } else if (err != ESP_OK) {
                        p3a_render_set_channel_message(display_name, P3A_CHANNEL_MSG_ERROR, -1, "Museum refresh failed");
                    }
                }
            }
        } else {
            // Gate 1: Require SNTP synchronization.
            // Without a valid clock, time comparisons are meaningless and any
            // saved timestamp would be garbage. Channels are re-queued once
            // SNTP synchronizes (one-shot below).
            if (!sntp_sync_is_synchronized()) {
                ESP_LOGI(TAG, "Makapix channel '%s' deferred (SNTP not synchronized)", display_name);
                err = ESP_OK;
            } else {
                // Gate 2: Check if channel was refreshed recently enough.
                char mkx_ch_path[128];
                channel_metadata_t mkx_meta = {0};
                if (sd_path_get_channel(mkx_ch_path, sizeof(mkx_ch_path)) == ESP_OK) {
                    channel_metadata_load(channel_id, mkx_ch_path, &mkx_meta);
                } else {
                    ESP_LOGE(TAG, "Cannot resolve channel directory; treating '%s' as never refreshed",
                             display_name);
                }

                time_t now = time(NULL);
                uint32_t interval = config_store_get_refresh_interval_sec();
                bool mkx_allow_override = config_store_get_refresh_allow_override();
                if (!mkx_allow_override &&
                    cache_has_entries &&
                    mkx_meta.last_refresh > 0 &&
                    (now - mkx_meta.last_refresh) < (time_t)interval) {
                    uint32_t remaining = interval - (uint32_t)(now - mkx_meta.last_refresh);
                    ESP_LOGI(TAG, "Makapix channel '%s' still fresh (last refresh %lds ago, interval %lus, stale in %lus), skipping",
                             display_name, (long)(now - mkx_meta.last_refresh), (unsigned long)interval, (unsigned long)remaining);
                    err = ESP_OK;
                } else {
                    if (mkx_allow_override) {
                        ESP_LOGI(TAG, "Channel '%s' refresh override active, bypassing interval check",
                                 display_name);
                    }
                    // Promoted channel: use HTTPS when MQTT is not connected
                    if (type == PS_CHANNEL_TYPE_NAMED &&
                        strcmp(spec_name, "promoted") == 0 &&
                        !makapix_mqtt_is_connected()) {
                        ESP_LOGI(TAG, "Using HTTPS fallback for promoted channel");
                        did_refresh = true;
                        err = makapix_promoted_https_refresh(channel_id);
                    } else {
                        err = refresh_makapix_channel(ch);
                    }
                }
            }
        }

        // Update state after refresh
        xSemaphoreTake(state->mutex, portMAX_DELAY);

        // Staleness guard: channel may have been reconfigured while we were refreshing
        if ((size_t)ch_idx >= state->channel_count ||
            strcmp(state->channels[ch_idx].channel_id, channel_id) != 0) {
            ESP_LOGW(TAG, "Channel '%s' reconfigured during refresh, discarding result", display_name);
            xSemaphoreGive(state->mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ch = &state->channels[ch_idx];

        size_t sync_entry_count = 0;
        if (err == ESP_ERR_INVALID_STATE) {
            // MQTT not connected - re-queue for retry when MQTT connects
            ch->refresh_in_progress = false;
            ch->refresh_pending = true;
            ESP_LOGD(TAG, "Channel '%s' queued for retry (MQTT not connected)", display_name);
        } else if (err == ESP_ERR_NOT_FINISHED) {
            // Makapix refresh started asynchronously - keep refresh_in_progress true
            // The async completion handler will update state when done
            ESP_LOGD(TAG, "Channel '%s' refresh started (async)", display_name);
            // Note: refresh_async_pending was set in refresh_makapix_channel()
        } else if (err == ESP_OK) {
            ch->refresh_in_progress = false;

            // Success (or clean no-op): clear the failure backoff.
            ch->fail_streak = 0;
            ch->retry_at_us = 0;

            // Bump in-memory last_refresh only when an actual refresh primitive
            // ran (did_refresh) and the wall clock is trustworthy. Mirrors the
            // SNTP-gated disk save in giphy_refresh.c and makapix_channel_refresh.c.
            // Pre-SNTP, time(NULL) returns boot-elapsed seconds — writing that
            // would make a just-refreshed channel look "older" than channels
            // refreshed last week with a real clock, breaking oldest-first.
            if (did_refresh && sntp_sync_is_synchronized()) {
                ch->last_refresh = time(NULL);
            }

            // A refresh actually ran: arm the pin_lists GC kick for the next
            // idle settle.
            if (did_refresh) {
                s_gc_kick_armed = true;
            }

            // Update active flag based on available artwork count
            if (ch->cache) {
                ch->active = (ch->cache->available_count > 0);
            }

            sync_entry_count = ch->cache ? ch->cache->entry_count : ch->entry_count;
            ESP_LOGI(TAG, "Channel '%s' refresh complete: %zu entries, active=%d",
                     display_name, sync_entry_count, ch->active);

            // Recalculate weights now that this channel has data
            ps_swrr_calculate_weights(state);

            // Reset download cursors so download manager rescans the new cache
            extern void download_manager_reset_cursors(void);
            download_manager_reset_cursors();

            // Signal that refresh is done - this wakes download task
            // which may be waiting for initial refresh to complete
            makapix_channel_signal_refresh_done();
        } else {
            ch->refresh_in_progress = false;
            if ((type == PS_CHANNEL_TYPE_GIPHY || type == PS_CHANNEL_TYPE_INSTITUTION) &&
                err == ESP_ERR_INVALID_RESPONSE) {
                // Giphy / institution rate-limit: the key/museum-wide cooldown
                // already gates eligibility — no per-channel backoff on top.
                // Keep pending so the channel retries within ~2s of the
                // cooldown clearing instead of waiting up to a full periodic
                // cycle. ESP_ERR_INVALID_RESPONSE is only ever returned by
                // network paths for HTTP 429.
                ch->refresh_pending = true;
            } else if (type == PS_CHANNEL_TYPE_GIPHY && err == ESP_ERR_NOT_ALLOWED &&
                       giphy_is_auth_invalid()) {
                // Invalid API key: the key-wide auth latch already gates
                // eligibility (cleared by key save, bypassed by the force
                // override, expiring into an hourly reprobe) — no per-channel
                // backoff on top, mirroring the 429 arm above. Cancellation
                // also travels as NOT_ALLOWED but never engages the latch,
                // so it still takes the generic backoff below.
                ch->refresh_pending = true;
            } else if (type == PS_CHANNEL_TYPE_GIPHY && err == ESP_ERR_NOT_FOUND &&
                       giphy_is_no_key()) {
                // Missing API key: parked key-wide just like the auth latch.
                // The giphy_is_no_key() conjunct keeps other NOT_FOUND
                // sources (e.g. a cache that vanished mid-refresh) on the
                // generic backoff below.
                ch->refresh_pending = true;
            } else {
                // Other failures (DNS, 5xx, timeout, dead artwork URL):
                // per-channel exponential backoff with pending
                // preserved — the eligibility gate holds the channel until
                // retry_at_us, then the periodic re-check retries it within
                // ~2 s. Previously pending was dropped and the periodic cycle
                // re-queued the stale channel ~10 s later, hammering a
                // failing endpoint indefinitely. Jitter (±20%) keeps channels
                // that failed together (e.g. a Wi-Fi blip mid-cycle) from
                // retrying in lockstep.
                uint32_t shift = ch->fail_streak < 5 ? ch->fail_streak : 5;
                uint32_t backoff_sec = RETRY_BACKOFF_BASE_SEC << shift;
                if (backoff_sec > RETRY_BACKOFF_CAP_SEC) backoff_sec = RETRY_BACKOFF_CAP_SEC;
                uint32_t backoff_ms = backoff_sec * (800 + (esp_random() % 401));
                ch->retry_at_us = esp_timer_get_time() + (int64_t)backoff_ms * 1000;
                if (ch->fail_streak < UINT8_MAX) ch->fail_streak++;
                ch->refresh_pending = true;
                ESP_LOGI(TAG, "Channel '%s' failure #%u, retry in ~%lus",
                         display_name, (unsigned)ch->fail_streak,
                         (unsigned long)(backoff_ms / 1000));
            }
        }

        // Check if we should trigger playback after refresh
        bool should_trigger_playback = (err == ESP_OK && sync_entry_count > 0 && ch->active && !state->first_swap_emitted);
        bool is_artwork_channel = (type == PS_CHANNEL_TYPE_ARTWORK);
        // SD card channel that refreshed cleanly but found no artworks: the
        // initial "Loading channel..." message will sit forever otherwise,
        // because there's no playback to trigger and nothing else clears it.
        bool sdcard_empty = (type == PS_CHANNEL_TYPE_SDCARD && err == ESP_OK && sync_entry_count == 0);

        xSemaphoreGive(state->mutex);

        if (sdcard_empty) {
            char anim_path[128];
            char detail[160];
            if (sd_path_get_animations(anim_path, sizeof(anim_path)) == ESP_OK) {
                // Drop the "/sdcard" mount prefix — the user thinks in terms of
                // the card's own filesystem, not our VFS mount point.
                const char *show_path = anim_path;
                const char *mount = CONFIG_BSP_SD_MOUNT_POINT;
                size_t mount_len = strlen(mount);
                if (strncmp(anim_path, mount, mount_len) == 0) {
                    show_path = anim_path + mount_len;
                }
                snprintf(detail, sizeof(detail), "No artworks found in\n%s", show_path);
            } else {
                // Don't name a directory we can't resolve — it would be a lie.
                snprintf(detail, sizeof(detail), "No artworks found");
            }
            p3a_render_set_channel_message(display_name, P3A_CHANNEL_MSG_ERROR, -1, detail);
        }

        if (should_trigger_playback) {
            ESP_LOGI(TAG, "%s - triggering playback",
                     is_artwork_channel ? "Artwork channel ready" :
                     (type == PS_CHANNEL_TYPE_GIPHY) ? "Giphy refresh complete" :
                     "Sync refresh complete");

            // For artwork channels: DON'T clear the message here.
            // The animation player will clear it AFTER the buffer swap completes,
            // ensuring seamless UI → new animation transition (no flash).
            // For other channels during normal operation: clear immediately.
            // During fresh start: keep the message until the buffer swap clears it.
            if (!is_artwork_channel) {
                if (animation_player_is_animation_ready()) {
                    p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                }
            }

            // Trigger playback
            if (play_scheduler_next(NULL) == ESP_OK) {
                xSemaphoreTake(state->mutex, portMAX_DELAY);
                state->first_swap_emitted = true;
                ps_assert_first_swap_invariant(state, "sync_refresh_complete");
                xSemaphoreGive(state->mutex);
            }
        }

        if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Channel '%s' refresh failed: %s", display_name, esp_err_to_name(err));
        }

        // Brief delay between refreshes to avoid overloading
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Refresh task exiting");
    // Park at a sync point so ps_refresh_stop can delete us externally; this
    // is the only safe way to free/reuse the static TCB and stack with
    // configSUPPORT_STATIC_ALLOCATION. Setting s_refresh_task = NULL before
    // vTaskDelete(NULL) would leave a window where the caller frees state
    // while FreeRTOS still holds the TCB in xTasksWaitingTermination.
    if (s_refresh_events) {
        xEventGroupSetBits(s_refresh_events, REFRESH_EVENT_PARKED);
    }
    vTaskSuspend(NULL);
    // Defensive: never reached.
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t ps_refresh_start(void)
{
    if (s_refresh_task != NULL) {
        ESP_LOGD(TAG, "Refresh task already running");
        return ESP_OK;
    }

    if (s_refresh_events == NULL) {
        s_refresh_events = xEventGroupCreate();
        if (s_refresh_events == NULL) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create refresh task with SPIRAM-backed stack
    if (!s_refresh_stack) {
        s_refresh_stack = heap_caps_malloc(REFRESH_TASK_STACK_SIZE * sizeof(StackType_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    // Pin to Core 0 to avoid interfering with animation rendering on Core 1
    bool task_created = false;
    if (s_refresh_stack) {
        s_refresh_task = xTaskCreateStaticPinnedToCore(refresh_task, "ps_refresh",
                                            REFRESH_TASK_STACK_SIZE, NULL, CONFIG_P3A_APP_TASK_PRIORITY,
                                            s_refresh_stack, &s_refresh_task_buffer, 0);
        task_created = (s_refresh_task != NULL);
    }

    if (!task_created) {
        if (xTaskCreatePinnedToCore(refresh_task, "ps_refresh",
                        REFRESH_TASK_STACK_SIZE, NULL, CONFIG_P3A_APP_TASK_PRIORITY, &s_refresh_task, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create refresh task");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Refresh task created");
    return ESP_OK;
}

void ps_refresh_stop(void)
{
    if (s_refresh_task == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Stopping refresh task");
    s_task_running = false;

    if (s_refresh_events) {
        xEventGroupSetBits(s_refresh_events, REFRESH_EVENT_SHUTDOWN);
    }

    TaskHandle_t handle = s_refresh_task;

    // Wait for the task to reach the parked sync point. The task sets
    // REFRESH_EVENT_PARKED immediately before vTaskSuspend(NULL).
    bool parked = false;
    if (s_refresh_events) {
        EventBits_t bits = xEventGroupWaitBits(s_refresh_events, REFRESH_EVENT_PARKED,
                                                pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        parked = (bits & REFRESH_EVENT_PARKED) != 0;
    }

    if (!parked) {
        ESP_LOGE(TAG, "ps_refresh task did not park within 5s; leaking task to preserve memory safety");
        // Leave s_refresh_task non-NULL so a future ps_refresh_start sees it
        // and skips creating a duplicate.
        return;
    }

    // Close the gap between event-set and vTaskSuspend(NULL).
    for (int i = 0; i < 100 && eTaskGetState(handle) != eSuspended; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    s_refresh_task = NULL;
    vTaskDelete(handle);

    // For static allocation, FreeRTOS does not free the TCB; the application
    // owns it. After vTaskDelete the task sits in xTasksWaitingTermination
    // until the pinned core's idle task runs prvCheckTasksWaitingTermination.
    // Yield for several ticks before this static buffer can be safely reused.
    vTaskDelay(pdMS_TO_TICKS(20));

    if (s_refresh_events) {
        vEventGroupDelete(s_refresh_events);
        s_refresh_events = NULL;
    }
}

void ps_refresh_signal_work(void)
{
    if (s_refresh_events) {
        xEventGroupSetBits(s_refresh_events, REFRESH_EVENT_WORK_AVAILABLE);
    }
}

void ps_refresh_reset_timer(void)
{
    // Reset the periodic refresh timer - called when a new playset is executed
    // This ensures immediate refresh happens and the 1-hour timer starts fresh
    s_last_full_refresh_complete = 0;
    s_next_refresh_delay = REFRESH_INTERVAL_SECONDS;
    s_pico8_was_active = false;
    s_gc_kick_armed = true;  // Kick pin_lists GC at the next idle settle
    ESP_LOGD(TAG, "Refresh timer reset");
}

// Helper to build cache path (exposed for use by refresh_task)
void ps_build_cache_path_internal(const char *channel_id, char *out_path, size_t max_len)
{
    if (!out_path || max_len == 0) return;

    char channel_dir[256];
    esp_err_t err = sd_path_get_channel(channel_dir, sizeof(channel_dir));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot resolve channel directory: %s", esp_err_to_name(err));
        out_path[0] = '\0';
        return;
    }

    // channel_id is a hex hash — always filesystem-safe, no sanitization needed
    snprintf(out_path, max_len, "%s/%s.bin", channel_dir, channel_id);
}
