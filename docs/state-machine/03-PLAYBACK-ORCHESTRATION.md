# Playback Orchestration Specification

## Overview

This document specifies how p3a orchestrates animation playback in response to Scheduler Commands (SC), integrating with channel refreshes and downloads while **never blocking playback**.

## Core Principle

> **Playback is king.** Background operations (refresh, download) are supporting actors that must never block or interrupt the user's viewing experience.

## Scheduler Command (SC) Lifecycle

### SC Reception

A Scheduler Command arrives via:
- User touch gesture (channel switch)
- MQTT message (e.g., `show_artwork`, `play_channel`)
- Web UI action
- Boot-time restore

### SC Processing Steps

```
1. Stop view tracking for old channel
2. Interrupt any in-flight channel refresh
3. Allow in-flight download to complete (don't interrupt)
4. Load LAi for channels in SC
5. Begin playback from LAi immediately
6. IF online mode: Trigger background refresh for SC channels
7. IF online mode: Signal download manager to scan for holes
```

### Step-by-Step Implementation

#### Step 1: Stop View Tracking

```c
// In play_scheduler_execute_command()
view_tracker_stop();  // Don't send views for wrong channel
```

**Rationale**: View tracking must be channel-specific. Stopping before switch prevents mis-attributed views.

#### Step 2: Interrupt Channel Refresh

```c
// In play_scheduler_execute_command()
ps_refresh_abort_current();  // Signal background task to stop

// In ps_refresh_task (background thread)
while (refreshing) {
    if (s_abort_flag) {
        ESP_LOGI(TAG, "Refresh aborted - SC changed");
        break;  // Clean exit
    }
    // Continue MQTT query...
}
```

**Rationale**: Refreshing the wrong channel wastes bandwidth and time. Clean abort prevents corrupted cache files.

#### Step 3: Allow In-Flight Download

```c
// Download manager runs independently
// No explicit wait - download completes asynchronously
// When download completes, it calls lai_add_entry() and signals playback
```

**Rationale**: Aborting downloads mid-flight can leave partial files. Better to finish and discard if no longer needed.

#### Step 4: Load LAi

```c
// In play_scheduler_execute_command()
for (size_t i = 0; i < command->channel_count; i++) {
    ps_channel_state_t *ch = &s_state.channels[i];
    
    // Build channel_id from spec
    ps_build_channel_id(&command->channels[i], ch->channel_id);
    
    // Load cache file (includes LAi)
    ps_load_channel_cache(ch);
    
    ESP_LOGI(TAG, "Channel '%s': lai_count=%zu", 
             ch->channel_id, ch->lai_count);
}
```

**Rationale**: LAi tells us what we can play right now. Fast O(1) lookup.

#### Step 5: Begin Playback

```c
// Check if any channel has entries
bool has_entries = false;
for (size_t i = 0; i < s_state.channel_count; i++) {
    if (s_state.channels[i].lai_count > 0) {
        has_entries = true;
        break;
    }
}

if (has_entries) {
    // Play immediately
    play_scheduler_next(NULL);
} else {
    // No entries - show informative message
    show_waiting_message();
}
```

**Key Decision**: If all LAi arrays are empty, show message but don't block. Background operations will populate LAi.

#### Step 6: Trigger Background Refresh (If Online)

```c
if (p3a_connectivity_is_online()) {
    // Mark channels as needing refresh
    for (size_t i = 0; i < command->channel_count; i++) {
        s_state.channels[i].refresh_pending = true;
    }
    
    // Wake refresh task
    ps_refresh_signal_work();
}
```

**Rationale**: Only refresh when online. Offline mode just plays what's available.

#### Step 7: Signal Download Manager

```c
if (p3a_connectivity_is_online()) {
    // Update download manager with new channel list
    const char *channel_ids[PS_MAX_CHANNELS];
    for (size_t i = 0; i < command->channel_count; i++) {
        channel_ids[i] = s_state.channels[i].channel_id;
    }
    download_manager_set_channels(channel_ids, command->channel_count);
    
    // Reset cursors to rescan from beginning
    download_manager_reset_cursors();
    
    // Wake download task
    download_manager_signal_work_available();
}
```

**Rationale**: Download manager independently scans channels for missing files.

## Playback States

### Empty LAi (No Playable Content)

**Scenario**: All channels have `lai_count == 0`

**Behavior**:
```c
// No playback starts yet
show_message("Loading channel...");

// Background refresh runs
// As entries arrive, LAi is built

// When first download completes:
download_complete_callback(entry) {
    lai_add_entry(channel, entry_idx);
    
    if (is_first_available_entry()) {
        // Trigger playback
        play_scheduler_next(NULL);
    }
}
```

**User Experience**:
```
T+0s:  "Loading channel..."
T+2s:  [First entry downloads]
T+2s:  [Animation starts playing]
T+10s: [Auto-swap to next entry]
```

### Partial LAi (Some Content Available)

**Scenario**: Some channels have `lai_count > 0`, others are empty

**Behavior**:
```c
// Playback starts immediately from channels with entries
play_scheduler_next(NULL);

// SWRR allocates credits based on lai_count
// Channels with lai_count=0 get weight=0 (skipped)

// As downloads complete, lai_count increases
// SWRR weights are recalculated
// More channels become playable
```

**User Experience**:
```
T+0s:  [Animation starts from channel A]
T+5s:  [Auto-swap within channel A]
T+10s: [Channel B's first entry downloads]
T+10s: [Next swap might pick from channel B]
```

### Full LAi (All Content Available)

**Scenario**: All channels have `lai_count > 0`

**Behavior**:
```c
// Normal operation
// SWRR rotates between channels
// Downloads continue in background for missing entries
```

## Background Refresh Behavior

### Refresh Task (ps_refresh_task)

```c
void ps_refresh_task(void *arg) {
    while (1) {
        // Wait for work signal
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Process channels needing refresh
        for (size_t i = 0; i < s_channel_count; i++) {
            if (!s_channels[i].refresh_pending) continue;
            if (s_abort_flag) break;  // SC changed
            
            ESP_LOGI(TAG, "Refreshing channel '%s'", s_channels[i].channel_id);
            
            // Query MQTT for channel entries
            esp_err_t err = makapix_refresh_channel_index(
                s_channels[i].type, s_channels[i].identifier);
            
            if (err == ESP_OK && !s_abort_flag) {
                // Reload cache (now includes new entries)
                ps_load_channel_cache(&s_channels[i]);
                
                // Mark refresh complete
                s_channels[i].refresh_pending = false;
                
                // Signal download manager (new entries may need downloading)
                download_manager_signal_work_available();
            }
        }
    }
}
```

**Key Features**:
- Runs on background task (8 KB stack)
- Checks abort flag between operations
- Doesn't hold mutex during MQTT query (allows concurrent playback)
- Signals download manager when complete

### Refresh Interruption

When SC changes mid-refresh:

```c
// New SC arrives
ps_refresh_abort_current();  // Set abort flag

// Refresh task checks flag
if (s_abort_flag) {
    ESP_LOGI(TAG, "Refresh aborted mid-query");
    break;  // Exit loop cleanly
}

// New SC starts fresh refresh
ps_refresh_signal_work();
```

**Guarantees**:
- No corrupted cache files (atomic writes)
- No wasted bandwidth (abort ASAP)
- No delayed response (playback starts immediately)

## Download Completion Hooks

When download manager completes a file:

```c
void on_download_complete(const char *channel_id, uint32_t ci_index) {
    // 1. Add to LAi
    ps_channel_state_t *ch = find_channel(channel_id);
    lai_add_entry(ch, ci_index);
    
    // 2. If this was the first entry, trigger playback
    if (ch->lai_count == 1 && !playback_active()) {
        ESP_LOGI(TAG, "First entry available - starting playback");
        play_scheduler_next(NULL);
    }
    
    // 3. Signal download manager (may be more work)
    download_manager_signal_work_available();
}
```

**Edge Cases Handled**:
- First entry for channel: Start playback
- Additional entries: Continue playback (natural SWRR rotation)
- Download for non-active channel: Ignore (but still add to LAi)

## Play Scheduler `next()` Behavior

### Algorithm with LAi

```c
esp_err_t play_scheduler_next(ps_artwork_t *out_artwork) {
    // 1. Select channel via SWRR (channels with lai_count=0 are skipped)
    size_t ch_idx = ps_swrr_select_channel(&s_state);
    if (ch_idx == SIZE_MAX) {
        // No active channels
        return ESP_ERR_NOT_FOUND;
    }
    
    ps_channel_state_t *ch = &s_state.channels[ch_idx];
    
    // 2. Pick from LAi (recency or random mode)
    uint32_t lai_idx = pick_from_lai(ch);  // 0 to lai_count-1
    uint32_t ci_idx = ch->lai_indices[lai_idx];
    channel_entry_t *entry = &ch->entries[ci_idx];
    
    // 3. Build artwork reference
    ps_artwork_t artwork;
    artwork.artwork_id = entry->post_id;
    build_filepath(entry, artwork.filepath);
    artwork.type = entry->extension;
    
    // 4. Request swap
    return prepare_and_request_swap(&artwork);
}
```

**No Retries Needed**: LAi guarantees file exists locally.

### Pick Modes

#### Recency Mode (PS_PICK_RECENCY)
```c
uint32_t pick_from_lai(ps_channel_state_t *ch) {
    uint32_t lai_idx = ch->lai_cursor % ch->lai_count;
    ch->lai_cursor++;
    return lai_idx;
}
```

Walks through LAi in order (newest to oldest if LAi is sorted by Ci).

#### Random Mode (PS_PICK_RANDOM)
```c
uint32_t pick_from_lai(ps_channel_state_t *ch) {
    uint32_t rand_val = ps_prng_next(&ch->pick_rng_state);
    return rand_val % ch->lai_count;
}
```

Picks uniformly from LAi.

## Informative Messages

### Message Priority

```
1. Connectivity issues (no Wi-Fi, no internet, not registered)
2. Channel-specific issues (loading, downloading, empty)
3. Normal playback (no message)
```

### Message Types

```c
typedef enum {
    MSG_NONE,                    // Normal playback
    MSG_NO_WIFI,                 // "No Wi-Fi"
    MSG_NO_INTERNET,             // "No Internet"
    MSG_NOT_REGISTERED,          // "Not registered"
    MSG_LOADING_CHANNEL,         // "Loading channel..."
    MSG_DOWNLOADING_FIRST,       // "Downloading artwork... (45%)"
    MSG_NO_ARTWORKS,             // "No artworks available"
} p3a_playback_message_t;
```

### Message Display Logic

```c
p3a_playback_message_t get_playback_message(void) {
    // Priority 1: Connectivity
    if (!p3a_connectivity_has_wifi()) {
        return MSG_NO_WIFI;
    }
    if (!p3a_connectivity_has_internet()) {
        return MSG_NO_INTERNET;
    }
    if (!p3a_connectivity_is_registered()) {
        return MSG_NOT_REGISTERED;
    }
    
    // Priority 2: Channel state
    bool any_lai_available = false;
    bool any_refresh_pending = false;
    bool any_download_active = false;
    
    for (size_t i = 0; i < s_channel_count; i++) {
        if (s_channels[i].lai_count > 0) {
            any_lai_available = true;
        }
        if (s_channels[i].refresh_pending) {
            any_refresh_pending = true;
        }
    }
    
    if (download_manager_is_busy()) {
        any_download_active = true;
    }
    
    if (any_lai_available) {
        return MSG_NONE;  // Playing normally
    }
    
    if (any_refresh_pending) {
        return MSG_LOADING_CHANNEL;
    }
    
    if (any_download_active) {
        return MSG_DOWNLOADING_FIRST;
    }
    
    // No LAi, no refresh, no download - truly empty
    return MSG_NO_ARTWORKS;
}
```

## Special Cases

### Case 1: All Channels Empty After Refresh

**Scenario**: Refresh completes, but no entries were returned (empty channel on server)

**Behavior**:
```c
if (refresh_complete && ch->entry_count == 0) {
    ch->lai_count = 0;
    show_message("Channel empty on server");
}
```

**User Experience**: Clear message, suggests trying a different channel.

### Case 2: Files Deleted Externally (USB Mass Storage)

**Scenario**: User connects p3a via USB, deletes files, disconnects

**Behavior**:
```c
// LAi now has stale entries
// During playback:
if (!file_exists(filepath)) {
    ESP_LOGW(TAG, "File missing (deleted externally?) - removing from LAi");
    lai_remove_entry(ch, ci_idx);
    
    // Try next entry
    return play_scheduler_next(out_artwork);
}
```

**Robustness**: System self-heals by removing stale LAi entries.

### Case 3: Rapid Channel Switching

**Scenario**: User switches channels rapidly (A → B → C → A)

**Behavior**:
```c
// Each switch:
// 1. Abort current refresh
// 2. Load LAi for new channel
// 3. Start playback immediately
// 4. Begin new refresh in background

// No queueing of operations
// Last switch wins
```

**User Experience**: Immediate response, no lag.

## Performance Considerations

### Playback Latency

**Target**: < 100ms from SC reception to first frame

**Breakdown**:
- Load LAi: ~10ms (read from SD card)
- Pick entry: ~1ms (O(1) array access)
- Request swap: ~5ms (animation_player overhead)
- Decode first frame: ~50ms (depends on image size)

**Total**: ~66ms ✅ Under target

### Refresh Latency

**Non-blocking**: Refresh runs on background task, doesn't delay playback.

**Typical Duration**: 500ms - 5s depending on channel size and MQTT latency.

### Download Latency

**Non-blocking**: Downloads run on separate task, one at a time.

**Typical Duration**: 1-10s per file depending on size and Wi-Fi speed.

## Open Questions

### Q1: Refresh Frequency
**Question**: How often should we refresh channels automatically?
**Options**:
- A) On SC only (user-initiated)
- B) Periodic every 60 minutes
- C) On MQTT notification (channel_updated event)

**Recommendation**: Option C with fallback to B. Real-time updates when available, periodic as backup.

### Q2: Download Priority
**Question**: Should download manager prioritize channels with smaller LAi?
**Options**:
- A) Round-robin (fair)
- B) Priority to channels with lai_count < 5 (ensure variety)
- C) Priority to currently playing channel

**Recommendation**: Option B. Ensures user sees variety quickly.

### Q3: Empty LAi Timeout
**Question**: How long to wait before showing "No artworks" if downloads are failing?
**Options**:
- A) 10 seconds
- B) 30 seconds
- C) Never (always show "Downloading...")

**Recommendation**: Option B. Balance between patience and feedback.

## Testing Strategy

### Unit Tests
- LAi operations (add, remove, contains)
- SWRR with mixed lai_count values
- Pick algorithms (recency, random)

### Integration Tests
- SC with all-empty LAi → first download → playback
- SC with partial LAi → immediate playback
- Rapid channel switching → abort + restart
- Refresh failure → playback continues

### Load Tests
- 16 channels, 1024 entries each → performance
- Rapid SC switches (10/second) → stability
- Large downloads during playback → no frame drops
