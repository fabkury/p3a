# p3a State Machine — Play Scheduler Behavior

**Document:** 03-PLAY-SCHEDULER-BEHAVIOR.md  
**Status:** Final Specification

---

## 1. High-Level Behavior

The Play Scheduler (PS) operates under these principles:

1. **Immediate Playback**: When a scheduler command (SC) arrives, begin playing immediately from available artworks
2. **LAi-Only Visibility**: PS only "sees" artworks in LAi, never just in Ci
3. **Background Operations**: Channel refresh and downloads happen without blocking playback
4. **Empty Channel Handling**: If all LAi's are empty, display informative messages
5. **Robustness**: Gracefully handle load failures by removing from LAi and trying another

---

## 2. Scheduler Command Processing

### 2.1 Command Reception Flow

```
┌──────────────────────────────────────────────────────────────────────┐
│                      New Scheduler Command (SC)                       │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 1. Interrupt ongoing channel refresh (if any)                         │
│    - Set abort flag                                                   │
│    - Wait for clean exit (max 500ms)                                  │
│    - In-flight download continues (not interrupted)                   │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 2. Load LAi for each channel in SC                                    │
│    - Read {channel_id}.bin from SD card                               │
│    - Extract LAi array from cache file                                │
│    - If cache missing: LAi = empty                                    │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 3. Check if any LAi is non-empty                                      │
│    ├─ YES → Begin playback immediately                                │
│    └─ NO  → Show informative message                                  │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 4. Schedule background work (if online)                               │
│    - Queue channel refresh for each channel in SC                     │
│    - Download manager begins filling availability holes               │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 Pseudo-code

```c
esp_err_t play_scheduler_execute_command(const ps_scheduler_command_t *command) {
    // 1. Abort ongoing refresh
    ps_refresh_abort_current();
    
    // 2. Load LAi for each channel
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    for (size_t i = 0; i < command->channel_count; i++) {
        ps_channel_state_t *ch = &s_state.channels[i];
        
        // Build channel_id from spec
        ps_build_channel_id(&command->channels[i], ch->channel_id);
        
        // Load cache with LAi
        esp_err_t err = ps_load_channel_cache_with_lai(ch);
        if (err != ESP_OK) {
            ch->available_count = 0;  // Empty LAi
        }
        
        ch->active = (ch->available_count > 0);
    }
    
    s_state.channel_count = command->channel_count;
    
    // 3. Check for available content
    bool has_content = false;
    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (s_state.channels[i].available_count > 0) {
            has_content = true;
            break;
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    
    // 4. Start playback or show message
    if (has_content) {
        return play_scheduler_next(NULL);
    } else {
        show_waiting_message();
    }
    
    // 5. Queue background work (always, even if has_content)
    if (connectivity_state_get() == CONN_STATE_ONLINE) {
        ps_refresh_queue_channels(command);
    }
    download_manager_set_channels(command);
    
    return ESP_OK;
}
```

---

## 3. LAi-Based Picking

### 3.1 The PS Only Sees LAi

```c
// BEFORE (current implementation):
static bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out) {
    // Iterates through Ci, checking file_exists() for each
    for (uint32_t attempts = 0; attempts < max_attempts; attempts++) {
        uint32_t ci_idx = pick_from_ci(state);
        if (is_file_available(ci_idx)) {  // <-- File I/O!
            *out = build_artwork(ci_idx);
            return true;
        }
    }
    return false;
}

// AFTER (with LAi):
static bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out) {
    // Pick directly from LAi - no file I/O
    ps_channel_state_t *ch = ps_swrr_select_channel(state);
    if (!ch || ch->available_count == 0) {
        return false;
    }
    
    uint32_t lai_idx;
    if (state->pick_mode == PS_PICK_RANDOM) {
        lai_idx = prng_next() % ch->available_count;
    } else {
        lai_idx = ch->cursor % ch->available_count;
        ch->cursor++;
    }
    
    uint32_t ci_idx = ch->available_indices[lai_idx];
    *out = build_artwork_from_ci(ch, ci_idx);
    return true;
}
```

### 3.2 No Skip Logic Needed

With LAi, the concept of "skipping unavailable artworks" during picking disappears:

- **Random pick**: Pick random index in LAi → no file check needed
- **Sequential pick**: Iterate through LAi → all entries assumed available
- **Multi-channel**: SWRR selects from channels with non-empty LAi

**However**, the system must handle load failures gracefully (see Section 8).

---

## 4. Informative Messages When LAi is Empty

### 4.1 Message States

When all channels have empty LAi:

```c
typedef enum {
    PS_WAIT_REFRESHING,    // "Refreshing channel index..."
    PS_WAIT_DOWNLOADING,   // "Downloading artwork..."
    PS_WAIT_NO_ARTWORKS,   // "No artworks available"
} ps_waiting_state_t;
```

### 4.2 Message Display Logic

```c
void update_waiting_message(void) {
    connectivity_state_t conn = connectivity_state_get();
    
    if (conn != CONN_STATE_ONLINE) {
        // Offline - show connectivity message
        set_message(connectivity_state_get_message(), "");
        return;
    }
    
    if (any_channel_refreshing()) {
        size_t items_received = get_refresh_item_count();
        set_message("Refreshing channel", "%zu items", items_received);
    }
    else if (download_manager_is_busy()) {
        int progress = get_download_progress_percent();
        set_message("Downloading artwork", "%d%%", progress);
    }
    else {
        set_message("No artworks", "Channel is empty");
    }
}
```

### 4.3 First Download Triggers Playback (Zero-to-One Transition)

When the first download completes during empty-LAi state, immediately begin playback:

```c
// In download_manager.c
void on_download_complete(const char *storage_key, uint32_t ci_index) {
    // Add to LAi
    lai_add_entry(active_channel, ci_index);
    
    // Check if we should trigger playback (zero-to-one transition)
    // This is different from "download completed while already playing"
    if (!s_playback_active && get_total_lai_count() > 0) {
        esp_err_t err = play_scheduler_next(NULL);
        if (err == ESP_OK) {
            s_playback_active = true;
            clear_waiting_message();
        }
    }
}
```

**Key distinction**: 
- During normal playback, new downloads do NOT interrupt — let dwell timer expire naturally
- When showing "waiting" message (LAi was empty), first download DOES trigger immediate playback

---

## 5. Background Channel Refresh

### 5.1 Non-Blocking Refresh

Channel refresh should never block playback:

```
┌─────────────────────────────────────────────────────────────────────┐
│                   Play Scheduler Task (foreground)                   │
│  - Picks artworks from LAi                                           │
│  - Handles next()/prev() calls                                       │
│  - Never waits for refresh                                           │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              │ signals work
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   Refresh Task (background)                          │
│  - Queries MQTT for channel entries                                  │
│  - Updates Ci (appends new entries)                                  │
│  - Does NOT modify LAi                                               │
│  - Signals download manager when new entries arrive                  │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              │ new entries available
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   Download Task (background)                         │
│  - Scans Ci for entries not in LAi (availability holes)              │
│  - Downloads files one at a time                                     │
│  - Updates LAi on download complete                                  │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 Refresh Scheduling

**On Scheduler Command Reception**:
1. All channels in the SC get refreshed immediately

**Periodic Refresh**:
1. When all channel refreshes from an SC complete as a group, start a 2-hour timer
2. When timer expires, refresh all channels in the current SC again
3. Timer resets if a new SC arrives

```c
// Refresh completion handler
void on_all_channels_refreshed(void) {
    // Start 2-hour timer for next refresh cycle
    start_refresh_timer(2 * 60 * 60 * 1000);  // 2 hours in ms
}

// Timer callback
void on_refresh_timer_expired(void) {
    if (connectivity_state_get() == CONN_STATE_ONLINE) {
        ps_refresh_queue_all_current_channels();
    }
}

// New SC received
void on_new_scheduler_command(void) {
    cancel_refresh_timer();  // Will restart when this round completes
    // ... process command ...
}
```

### 5.3 Early Download Start

Downloads start as soon as there are entries in Ci, not waiting for full refresh:

```c
// In refresh task, after receiving batch of entries
void on_refresh_batch_received(size_t new_entry_count) {
    // Persist Ci (but not LAi - that's download manager's job)
    cache_save_ci_only(channel);
    
    // Signal download manager immediately
    download_manager_signal_work_available();
}
```

### 5.4 Refresh Interruption

When a new SC arrives, ongoing refresh should abort cleanly:

```c
// Refresh task checks abort flag between MQTT pages
while (has_more_pages && !refresh_abort_requested()) {
    esp_err_t err = mqtt_query_next_page(&entries, &count);
    if (err != ESP_OK) break;
    
    append_to_ci(entries, count);
    download_manager_signal_work_available();
}

if (refresh_abort_requested()) {
    ESP_LOGI(TAG, "Refresh aborted due to new command");
    // Clean exit - Ci has partial data, which is fine
}
```

---

## 6. SWRR with LAi

### 6.1 Weight Calculation

Only channels with non-empty LAi participate in SWRR. Use equal weights for active channels:

```c
void ps_swrr_calculate_weights(ps_state_t *state) {
    size_t active_count = 0;
    
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].available_count > 0) {
            active_count++;
        } else {
            state->channels[i].weight = 0;  // Excluded from SWRR
        }
    }
    
    if (active_count == 0) return;
    
    // Equal weight for active channels (LAi size is NOT considered)
    uint32_t weight_per_channel = SWRR_WEIGHT_SUM / active_count;
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].available_count > 0) {
            state->channels[i].weight = weight_per_channel;
        }
    }
}
```

### 6.2 Dynamic Weight Updates

When LAi changes (file downloaded/evicted), recalculate weights if a channel crosses zero:

```c
void on_lai_changed(ps_channel_state_t *channel) {
    bool was_active = (channel->weight > 0);
    bool now_active = (channel->available_count > 0);
    
    if (was_active != now_active) {
        // Channel crossed zero boundary - recalculate all weights
        ps_swrr_calculate_weights(&s_state);
    }
}
```

---

## 7. Download Manager Channel Selection

When picking the next file availability hole to fill, the download manager uses **simple round-robin** across channels (not weighted):

```c
static size_t s_download_channel_cursor = 0;

esp_err_t download_manager_get_next(download_request_t *req) {
    size_t start = s_download_channel_cursor;
    
    do {
        channel_cache_t *ch = &s_channels[s_download_channel_cursor];
        s_download_channel_cursor = (s_download_channel_cursor + 1) % s_channel_count;
        
        // Find first downloadable entry in this channel
        uint32_t ci_idx;
        if (find_next_download_hole(ch, &ci_idx)) {
            build_download_request(ch, ci_idx, req);
            return ESP_OK;
        }
    } while (s_download_channel_cursor != start);
    
    return ESP_ERR_NOT_FOUND;  // All channels fully downloaded
}
```

---

## 8. Handling Load Failures (Robustness)

### 8.1 Load Failure Causes

Files in LAi may fail to load due to:
- File was deleted externally (SD card removed, user deleted)
- File is corrupted (incomplete download, SD card error)
- File format not supported (wrong file type downloaded)

### 8.2 Load Failure Handler

```c
void on_artwork_load_failed(ps_artwork_t *artwork, const char *reason) {
    ESP_LOGW(TAG, "Failed to load %s: %s", artwork->storage_key, reason);
    
    // 1. Record failure in LTF (may create terminal marker after 3 failures)
    ltf_record_failure(artwork->storage_key, reason);
    
    // 2. Delete the corrupted file (if it exists)
    char filepath[256];
    build_vault_filepath(artwork->storage_key, filepath, sizeof(filepath));
    unlink(filepath);
    
    // 3. Remove from LAi
    channel_cache_t *cache = get_channel_cache(artwork->channel_id);
    uint32_t ci_idx = find_ci_index(cache, artwork->storage_key);
    if (ci_idx != UINT32_MAX) {
        lai_remove_entry(cache, ci_idx);
    }
    
    // 4. Try to pick and play another artwork
    size_t total_available = get_total_lai_count_all_channels();
    
    if (total_available > 0) {
        // Still have content - pick another
        play_scheduler_next(NULL);
    } else {
        // LAi is now empty
        if (connectivity_state_get() == CONN_STATE_ONLINE) {
            show_message("Downloading artworks...", "");
            download_manager_signal_work_available();
        } else {
            show_message("No artworks available", "");
        }
    }
}
```

### 8.3 Preventing Infinite Loops

The LTF system (see 02-LOCALLY-AVAILABLE-INDEX.md) prevents infinite re-download loops:
- First 2 failures: File deleted, LTF updated, re-download allowed
- Third failure: File deleted, terminal LTF created, re-download blocked

---

## 9. History Behavior with LAi

History buffer stores artwork references, not LAi indices. This means:

- History entries remain valid even if LAi changes
- `prev()` plays from history regardless of current LAi state
- After exhausting history, `next()` picks from current LAi

```c
esp_err_t play_scheduler_prev(ps_artwork_t *out_artwork) {
    // prev() only walks history - never touches LAi
    if (!ps_history_can_go_back(&s_state)) {
        return ESP_ERR_NOT_FOUND;
    }
    
    ps_history_go_back(&s_state, out_artwork);
    return prepare_and_request_swap(out_artwork);
}
```

**Note**: History entries may point to files that no longer exist. If `prev()` fails to load, handle as a load failure (Section 8).

---

*Previous: [02-LOCALLY-AVAILABLE-INDEX.md](02-LOCALLY-AVAILABLE-INDEX.md)*  
*Next: [05-MIGRATION-PLAN.md](05-MIGRATION-PLAN.md)*
