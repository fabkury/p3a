// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_channel_switch.c
 * @brief Makapix channel switching, pending-channel queue, and show-artwork
 */

#include "makapix_internal.h"
#include "config_store.h"
#include "event_bus.h"

// Forward declaration to avoid pulling in play_scheduler.h (which has heavy deps)
esp_err_t play_scheduler_play_artwork(int32_t post_id, const char *storage_key, const char *art_url);

// Helper to map global play_order setting to channel_order_mode
static channel_order_mode_t get_global_channel_order(void)
{
    uint8_t play_order = config_store_get_play_order();
    switch (play_order) {
        case 1: return CHANNEL_ORDER_CREATED;
        case 2: return CHANNEL_ORDER_RANDOM;
        case 0:
        default:
            return CHANNEL_ORDER_ORIGINAL;
    }
}

// --------------------------------------------------------------------------
// Public API - Channel switching
// --------------------------------------------------------------------------

esp_err_t makapix_switch_to_channel(const char *channel, const char *identifier, const char *display_handle)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build channel ID
    char channel_id[128] = {0};
    if (strcmp(channel, "by_user") == 0) {
        if (!identifier || strlen(identifier) == 0) {
            ESP_LOGE(MAKAPIX_TAG, "identifier required for by_user channel");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(channel_id, sizeof(channel_id), "by_user_%s", identifier);
    } else if (strcmp(channel, "hashtag") == 0) {
        if (!identifier || strlen(identifier) == 0) {
            ESP_LOGE(MAKAPIX_TAG, "identifier required for hashtag channel");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(channel_id, sizeof(channel_id), "hashtag_%s", identifier);
    } else {
        strncpy(channel_id, channel, sizeof(channel_id) - 1);
    }

    // Check if we're already on this channel
    if (s_current_channel_id[0] && strcmp(s_current_channel_id, channel_id) == 0 && s_current_channel) {
        ESP_LOGI(MAKAPIX_TAG, "Already on channel %s, restarting playback without refresh", channel_id);
        // Restart playback but don't re-trigger refresh
        esp_err_t err = channel_start_playback(s_current_channel, get_global_channel_order(), NULL);
        if (err != ESP_OK) {
            ESP_LOGW(MAKAPIX_TAG, "Failed to restart playback: %s", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    // Build channel name for UI display
    // For by_user: use display_handle if provided, otherwise fall back to identifier
    char channel_name[128] = {0};
    if (strcmp(channel, "all") == 0) {
        strcpy(channel_name, "Recent");
    } else if (strcmp(channel, "promoted") == 0) {
        strcpy(channel_name, "Promoted");
    } else if (strcmp(channel, "user") == 0) {
        strcpy(channel_name, "My Artworks");
    } else if (strcmp(channel, "by_user") == 0) {
        const char *display = (display_handle && display_handle[0]) ? display_handle : identifier;
        snprintf(channel_name, sizeof(channel_name), "@%s's Artworks", display);
    } else if (strcmp(channel, "hashtag") == 0) {
        snprintf(channel_name, sizeof(channel_name), "#%s", identifier);
    } else {
        size_t copy_len = strlen(channel_id);
        if (copy_len >= sizeof(channel_name)) copy_len = sizeof(channel_name) - 1;
        memcpy(channel_name, channel_id, copy_len);
        channel_name[copy_len] = '\0';
    }

    // Store previous channel ID for error fallback
    strncpy(s_previous_channel_id, s_current_channel_id, sizeof(s_previous_channel_id) - 1);
    s_previous_channel_id[sizeof(s_previous_channel_id) - 1] = '\0';

    // Mark channel as loading (clear any previous abort state)
    s_channel_loading = true;
    s_channel_load_abort = false;
    strncpy(s_loading_channel_id, channel_id, sizeof(s_loading_channel_id) - 1);
    s_loading_channel_id[sizeof(s_loading_channel_id) - 1] = '\0';

    ESP_LOGD(MAKAPIX_TAG, "Switching to channel: %s", channel_name);

    // Destroy existing channel if any
    if (s_current_channel) {
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }

    // Get dynamic paths
    char vault_path[128], channel_path[128];
    sd_path_get_vault(vault_path, sizeof(vault_path));
    sd_path_get_channel(channel_path, sizeof(channel_path));

    // Create new Makapix channel
    s_current_channel = makapix_channel_create(channel_id, channel_name, vault_path, channel_path);
    if (!s_current_channel) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create channel");
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        return ESP_ERR_NO_MEM;
    }

    // Update current channel ID tracker
    strncpy(s_current_channel_id, channel_id, sizeof(s_current_channel_id) - 1);
    s_current_channel_id[sizeof(s_current_channel_id) - 1] = '\0';

    // Load channel (will trigger refresh task if index is empty)
    esp_err_t err = channel_load(s_current_channel);

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        // Serious error (e.g., refresh task couldn't start due to memory)
        ESP_LOGE(MAKAPIX_TAG, "Channel load failed: %s", esp_err_to_name(err));
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_ERROR, -1,
                                       "Failed to load channel");
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        s_current_channel_id[0] = '\0';
        p3a_state_fallback_to_sdcard();
        return err;
    }

    // Show "Connecting..." message if MQTT not yet connected
    // The refresh task is waiting for MQTT, so let the user know what's happening
    // But only if we have WiFi connectivity (no point in AP mode)
    if (!makapix_mqtt_is_connected() && p3a_state_has_wifi()) {
        ESP_LOGD(MAKAPIX_TAG, "MQTT not connected, showing 'Connecting...' message");
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_LOADING, -1,
                                       "Connecting to Makapix Club...");
        p3a_channel_message_t msg = {
            .type = P3A_CHANNEL_MSG_LOADING,
            .progress_percent = -1
        };
        snprintf(msg.channel_name, sizeof(msg.channel_name), "%s", channel_name);
        snprintf(msg.detail, sizeof(msg.detail), "Connecting to Makapix Club...");
        p3a_state_set_channel_message(&msg);
    }

    // Get channel stats - total_items is index entries (not necessarily downloaded)
    channel_stats_t stats = {0};
    channel_get_stats(s_current_channel, &stats);

    // Count locally AVAILABLE artworks (files that actually exist)
    // This is different from total_items which includes index entries without files
    size_t available_count = 0;
    size_t post_count = channel_get_post_count(s_current_channel);
    for (size_t i = 0; i < post_count; i++) {
        channel_post_t post = {0};
        if (channel_get_post(s_current_channel, i, &post) == ESP_OK) {
            if (post.kind == CHANNEL_POST_KIND_ARTWORK) {
                struct stat st;
                if (stat(post.u.artwork.filepath, &st) == 0) {
                    available_count++;
                }
            }
        }
    }

    ESP_LOGD(MAKAPIX_TAG, "Channel %s: %zu entries, %zu available",
             channel_id, stats.total_items, available_count);

    // Display title for UI messages is always "Makapix Club"
    const char *ui_title = "Makapix Club";

    // Decision: show loading UI only if ZERO artworks are locally available
    if (available_count == 0) {

        // Set up loading message UI based on channel state
        // IMPORTANT: Do NOT switch display render mode here. We keep the display in animation mode
        // and rely on p3a_render to draw the message reliably (avoids blank screen if UI mode fails).
        // Only show loading messages if we have WiFi connectivity (no point in AP mode)
        if (p3a_state_has_wifi()) {
            const char *loading_message;
            if (stats.total_items == 0) {
                // Empty index - waiting for refresh to populate
                loading_message = "Updating channel index...";
                ugfx_ui_show_channel_message(ui_title, loading_message, -1);
                p3a_render_set_channel_message(ui_title, P3A_CHANNEL_MSG_LOADING, -1, loading_message);
            } else {
                // Has index but no downloaded files yet - show "Waiting for download..."
                // The actual "Downloading artwork..." will be shown when download starts
                loading_message = "Waiting for download...";
                ugfx_ui_show_channel_message(ui_title, loading_message, -1);
                p3a_render_set_channel_message(ui_title, P3A_CHANNEL_MSG_DOWNLOADING, -1, loading_message);
            }
        }

        // Wait for FIRST artwork to become available using polling approach
        // We poll every 500ms to check for available files and update UI every 2 seconds
        const int MAX_WAIT_MS = 60000;
        const int POLL_INTERVAL_MS = 500;
        const int UI_UPDATE_INTERVAL_MS = 2000;
        int64_t start_time = esp_timer_get_time() / 1000;  // Convert to ms
        int64_t last_ui_update = 0;
        bool aborted = false;
        bool got_artwork = false;

        while (!aborted && !got_artwork) {
            int64_t now = esp_timer_get_time() / 1000;
            int64_t elapsed_ms = now - start_time;

            // Check timeout
            if (elapsed_ms >= MAX_WAIT_MS) {
                ESP_LOGW(MAKAPIX_TAG, "Timed out waiting for first artwork after %lld ms", (long long)elapsed_ms);
                break;
            }

            // Check if playback has already started (play_scheduler may have triggered it)
            extern bool animation_player_is_animation_ready(void) __attribute__((weak));
            if (animation_player_is_animation_ready && animation_player_is_animation_ready()) {
                ESP_LOGI(MAKAPIX_TAG, "Playback already started, exiting wait loop");
                got_artwork = true;  // Don't show any more messages
                break;
            }

            // Check for abort signal first for responsiveness
            if (s_channel_load_abort || s_has_pending_channel) {
                ESP_LOGD(MAKAPIX_TAG, "Channel load aborted by new request");
                aborted = true;
                break;
            }

            // Check for available artwork
            size_t new_post_count = channel_get_post_count(s_current_channel);
            for (size_t i = 0; i < new_post_count && !got_artwork; i++) {
                channel_post_t post = {0};
                if (channel_get_post(s_current_channel, i, &post) == ESP_OK) {
                    if (post.kind == CHANNEL_POST_KIND_ARTWORK) {
                        struct stat st;
                        if (stat(post.u.artwork.filepath, &st) == 0) {
                            got_artwork = true;  // Found one! Can start playback
                            ESP_LOGI(MAKAPIX_TAG, "First artwork available after %lld ms", (long long)elapsed_ms);
                        }
                    }
                }
            }

            if (got_artwork) {
                break;
            }

            // Update loading message every UI_UPDATE_INTERVAL_MS
            if (elapsed_ms - last_ui_update >= UI_UPDATE_INTERVAL_MS) {
                last_ui_update = elapsed_ms;
                size_t current_total = channel_get_post_count(s_current_channel);
                char msg[64];
                int msg_type;
                if (current_total == 0) {
                    snprintf(msg, sizeof(msg), "Updating index... (%lld sec)", (long long)(elapsed_ms / 1000));
                    msg_type = P3A_CHANNEL_MSG_LOADING;
                } else {
                    // Check if download is actively happening
                    if (download_manager_is_busy()) {
                        snprintf(msg, sizeof(msg), "Downloading artwork...");
                    } else {
                        snprintf(msg, sizeof(msg), "Waiting for download...");
                    }
                    msg_type = P3A_CHANNEL_MSG_DOWNLOADING;
                }
                ugfx_ui_show_channel_message(ui_title, msg, -1);
                p3a_render_set_channel_message(ui_title, msg_type, -1, msg);
            }

            // Wait a bit before checking again
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        }

        // Clear loading message
        ugfx_ui_hide_channel_message();
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);

        // Handle abort
        if (aborted) {
            channel_destroy(s_current_channel);
            s_current_channel = NULL;
            s_channel_loading = false;
            s_loading_channel_id[0] = '\0';
            s_current_channel_id[0] = '\0';
            s_channel_load_abort = false;

            char pending_ch[64] = {0};
            char pending_id[64] = {0};
            char pending_disp[64] = {0};
            if (makapix_get_pending_channel(pending_ch, sizeof(pending_ch),
                                             pending_id, sizeof(pending_id),
                                             pending_disp, sizeof(pending_disp))) {
                makapix_clear_pending_channel();
                return makapix_switch_to_channel(pending_ch,
                                                  pending_id[0] ? pending_id : NULL,
                                                  pending_disp[0] ? pending_disp : NULL);
            }
            return ESP_ERR_INVALID_STATE;
        }

        // Handle timeout
        if (!got_artwork) {
            ESP_LOGW(MAKAPIX_TAG, "Timed out waiting for artwork");

            // Clean up current channel
            channel_destroy(s_current_channel);
            s_current_channel = NULL;
            s_channel_loading = false;
            s_loading_channel_id[0] = '\0';
            s_current_channel_id[0] = '\0';

            // Check for pending channel first
            char pending_ch[64] = {0};
            char pending_id[64] = {0};
            char pending_disp[64] = {0};
            if (makapix_get_pending_channel(pending_ch, sizeof(pending_ch),
                                             pending_id, sizeof(pending_id),
                                             pending_disp, sizeof(pending_disp))) {
                makapix_clear_pending_channel();
                // Clear loading message before switching
                ugfx_ui_hide_channel_message();
                p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                return makapix_switch_to_channel(pending_ch,
                                                  pending_id[0] ? pending_id : NULL,
                                                  pending_disp[0] ? pending_disp : NULL);
            }

            // Fall back to previous channel if available
            if (s_previous_channel_id[0] != '\0') {
                // Clear loading message before switching
                ugfx_ui_hide_channel_message();
                p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                // Parse previous channel to extract channel type and identifier
                // Note: We don't have display_handle for previous channel, so pass NULL
                char prev_channel[64] = {0};
                char prev_identifier[64] = {0};
                if (strncmp(s_previous_channel_id, "by_user_", 8) == 0) {
                    snprintf(prev_channel, sizeof(prev_channel), "by_user");
                    snprintf(prev_identifier, sizeof(prev_identifier), "%.63s", s_previous_channel_id + 8);
                } else if (strncmp(s_previous_channel_id, "hashtag_", 8) == 0) {
                    snprintf(prev_channel, sizeof(prev_channel), "hashtag");
                    snprintf(prev_identifier, sizeof(prev_identifier), "%.63s", s_previous_channel_id + 8);
                } else {
                    snprintf(prev_channel, sizeof(prev_channel), "%.63s", s_previous_channel_id);
                }
                return makapix_switch_to_channel(prev_channel,
                                                  prev_identifier[0] ? prev_identifier : NULL,
                                                  NULL);
            }

            // No previous channel - fall back to SD card
            // Don't clear channel message - let fallback function show appropriate message
            // (it will show "No artworks available" if SD card is also empty)
            p3a_state_fallback_to_sdcard();
            return ESP_ERR_NOT_FOUND;
        }
    }

    // At this point we have at least one locally available artwork - start playback immediately!
    // Background downloads will continue adding more artworks

    // Start playback with global play order setting
    err = channel_start_playback(s_current_channel, get_global_channel_order(), NULL);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to start playback: %s", esp_err_to_name(err));
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        s_current_channel_id[0] = '\0';
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_ERROR, -1,
                                       "Failed to start playback");
        p3a_state_fallback_to_sdcard();
        return err;
    }

    // Switch play_scheduler to this channel and start playback
    if (strcmp(channel, "by_user") == 0 && identifier) {
        err = play_scheduler_play_user_channel(identifier);
    } else if (strcmp(channel, "hashtag") == 0 && identifier) {
        err = play_scheduler_play_hashtag_channel(identifier);
    } else {
        // "all", "promoted", or other named channels
        err = play_scheduler_play_named_channel(channel);
    }
    if (err != ESP_OK) {
        ESP_LOGW(MAKAPIX_TAG, "Failed to initiate play_scheduler: %s", esp_err_to_name(err));
    }

    ESP_LOGD(MAKAPIX_TAG, "Channel %s ready, playback initiated", channel_name);

    // Clear loading state - playback started
    s_channel_loading = false;
    s_loading_channel_id[0] = '\0';

    // Persist "last channel" selection
    if (strcmp(channel, "all") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_ALL, NULL);
    } else if (strcmp(channel, "promoted") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_PROMOTED, NULL);
    } else if (strcmp(channel, "user") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_USER, NULL);
    } else if (strcmp(channel, "by_user") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_BY_USER, identifier);
    } else if (strcmp(channel, "hashtag") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_HASHTAG, identifier);
    } else {
        ESP_LOGW(MAKAPIX_TAG, "Not persisting unknown channel key: %s", channel);
    }

    return ESP_OK;
}

// --------------------------------------------------------------------------
// Show-artwork helper functions
// --------------------------------------------------------------------------

/**
 * @brief Detect asset type from file path extension
 */
static asset_type_t detect_asset_type_from_path(const char *filepath)
{
    if (!filepath) return ASSET_TYPE_WEBP;

    size_t len = strlen(filepath);
    if (len >= 5 && strcasecmp(filepath + len - 5, ".webp") == 0) return ASSET_TYPE_WEBP;
    if (len >= 4 && strcasecmp(filepath + len - 4, ".gif") == 0)  return ASSET_TYPE_GIF;
    if (len >= 4 && strcasecmp(filepath + len - 4, ".png") == 0)  return ASSET_TYPE_PNG;
    if (len >= 4 && strcasecmp(filepath + len - 4, ".jpg") == 0)  return ASSET_TYPE_JPEG;
    if (len >= 5 && strcasecmp(filepath + len - 5, ".jpeg") == 0) return ASSET_TYPE_JPEG;

    return ASSET_TYPE_WEBP;  // Default
}

/**
 * @brief Show a single artwork by storage_key
 *
 * Delegates to play_scheduler_play_artwork() which creates an artwork channel
 * and handles download/playback through the unified scheduler path.
 */
esp_err_t makapix_show_artwork(int32_t post_id, const char *storage_key, const char *art_url)
{
    if (!storage_key || !art_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(MAKAPIX_TAG, "show_artwork: delegating to play_scheduler (post_id=%ld)", (long)post_id);

    // Mark that we're now on a single-artwork "channel" - this ensures that
    // subsequent channel switch requests (e.g., back to "promoted") will trigger
    // an actual switch instead of being ignored as "already on this channel"
    strlcpy(s_current_channel_id, "artwork", sizeof(s_current_channel_id));
    if (s_current_channel) {
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }

    return play_scheduler_play_artwork(post_id, storage_key, art_url);
}

void makapix_adopt_channel_handle(void *channel)
{
    // NOTE: `channel_handle_t` is opaque and defined in channel_manager. We take void* here to keep makapix.h lightweight.
    // Ownership transfer: if a different channel is already owned, destroy it.
    channel_handle_t ch = (channel_handle_t)channel;
    if (s_current_channel && s_current_channel != ch) {
        channel_destroy(s_current_channel);
    }
    s_current_channel = ch;

    // Track the channel ID for refresh triggering when MQTT connects
    if (ch) {
        const char *id = makapix_channel_get_id(ch);
        if (id) {
            strncpy(s_current_channel_id, id, sizeof(s_current_channel_id) - 1);
            s_current_channel_id[sizeof(s_current_channel_id) - 1] = '\0';
        }
    } else {
        s_current_channel_id[0] = '\0';
    }
}

bool makapix_is_channel_loading(char *out_channel_id, size_t max_len)
{
    if (s_channel_loading && out_channel_id && max_len > 0) {
        strncpy(out_channel_id, s_loading_channel_id, max_len - 1);
        out_channel_id[max_len - 1] = '\0';
    }
    return s_channel_loading;
}

void makapix_abort_channel_load(void)
{
    if (s_channel_loading) {
        s_channel_load_abort = true;
    }
}

esp_err_t makapix_request_channel_switch(const char *channel, const char *identifier, const char *display_handle)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build channel_id for comparison
    char new_channel_id[128] = {0};
    if (strcmp(channel, "by_user") == 0 && identifier) {
        snprintf(new_channel_id, sizeof(new_channel_id), "by_user_%s", identifier);
    } else if (strcmp(channel, "hashtag") == 0 && identifier) {
        snprintf(new_channel_id, sizeof(new_channel_id), "hashtag_%s", identifier);
    } else {
        strncpy(new_channel_id, channel, sizeof(new_channel_id) - 1);
    }

    // Check if this is the same channel already loading
    if (s_channel_loading && strcmp(s_loading_channel_id, new_channel_id) == 0) {
        return ESP_OK;
    }

    // Store as pending channel
    strncpy(s_pending_channel, channel, sizeof(s_pending_channel) - 1);
    s_pending_channel[sizeof(s_pending_channel) - 1] = '\0';

    if (identifier) {
        strncpy(s_pending_identifier, identifier, sizeof(s_pending_identifier) - 1);
        s_pending_identifier[sizeof(s_pending_identifier) - 1] = '\0';
    } else {
        s_pending_identifier[0] = '\0';
    }

    if (display_handle) {
        strncpy(s_pending_display_handle, display_handle, sizeof(s_pending_display_handle) - 1);
        s_pending_display_handle[sizeof(s_pending_display_handle) - 1] = '\0';
    } else {
        s_pending_display_handle[0] = '\0';
    }

    s_has_pending_channel = true;

    if (s_channel_loading) {
        s_channel_load_abort = true;
    } else {
        // No channel loading - signal the task to start processing
        if (s_channel_switch_sem) {
            xSemaphoreGive(s_channel_switch_sem);
        }
    }

    return ESP_OK;
}

bool makapix_has_pending_channel(void)
{
    return s_has_pending_channel;
}

bool makapix_get_pending_channel(char *out_channel, size_t channel_len,
                                  char *out_identifier, size_t id_len,
                                  char *out_display_handle, size_t handle_len)
{
    if (!s_has_pending_channel) {
        return false;
    }

    if (out_channel && channel_len > 0) {
        snprintf(out_channel, channel_len, "%s", s_pending_channel);
    }

    if (out_identifier && id_len > 0) {
        snprintf(out_identifier, id_len, "%s", s_pending_identifier);
    }

    if (out_display_handle && handle_len > 0) {
        snprintf(out_display_handle, handle_len, "%s", s_pending_display_handle);
    }

    return true;
}

void makapix_clear_pending_channel(void)
{
    s_has_pending_channel = false;
    s_pending_channel[0] = '\0';
    s_pending_identifier[0] = '\0';
    s_pending_display_handle[0] = '\0';
}

void makapix_clear_current_channel(void)
{
    // Clear the current channel ID so we don't skip switching back to the same channel later
    // This should be called when switching away from Makapix (e.g., to SD card)
    s_current_channel_id[0] = '\0';
    s_current_channel = NULL;  // Don't destroy - ownership may have been transferred
    ESP_LOGD(MAKAPIX_TAG, "Cleared current Makapix channel state");
}
