# Implementation Plan

## Overview

This document outlines the concrete code changes needed to implement the state machine refactoring, including file-by-file changes, migration strategy, and pain points to watch for.

## Phase 1: Add LAi Support to Channel Cache Format

### Goal
Modify channel cache file format to include LAi section while maintaining backward compatibility.

### Files to Modify

#### 1. `components/play_scheduler/include/play_scheduler_types.h`

**Add**:
```c
// LAi (Locally Available Index) format
typedef struct {
    uint32_t entry_count;      // Number of entries in Ci
    uint32_t lai_count;        // Number of available entries
    uint32_t lai_indices[];    // Flexible array: indices into Ci
} ps_channel_cache_header_t;

// Per-channel LAi tracking
typedef struct ps_channel_state_s {
    // ... existing fields ...
    
    // LAi support
    uint32_t *lai_indices;     // Array of indices into entries[]
    uint32_t lai_count;        // Number of locally available entries
    uint32_t lai_cursor;       // Current position in LAi (for iteration)
} ps_channel_state_t;
```

#### 2. `components/play_scheduler/play_scheduler_cache.c`

**New file** for cache I/O operations:

```c
// Write Ci + LAi atomically
esp_err_t ps_save_channel_cache(const ps_channel_state_t *ch) {
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", ch->cache_path);
    
    FILE *f = fopen(temp_path, "wb");
    if (!f) return ESP_FAIL;
    
    // Write header
    fwrite(&ch->entry_count, sizeof(uint32_t), 1, f);
    fwrite(&ch->lai_count, sizeof(uint32_t), 1, f);
    
    // Write Ci
    fwrite(ch->entries, ch->entry_size, ch->entry_count, f);
    
    // Write LAi
    fwrite(ch->lai_indices, sizeof(uint32_t), ch->lai_count, f);
    
    fclose(f);
    
    // Atomic rename
    if (rename(temp_path, ch->cache_path) != 0) {
        unlink(temp_path);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Load Ci + LAi
esp_err_t ps_load_channel_cache(ps_channel_state_t *ch) {
    FILE *f = fopen(ch->cache_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    
    // Read header
    uint32_t entry_count, lai_count;
    if (fread(&entry_count, sizeof(uint32_t), 1, f) != 1 ||
        fread(&lai_count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return ESP_FAIL;
    }
    
    // Validate
    if (entry_count > MAX_ENTRIES || lai_count > entry_count) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Allocate Ci
    ch->entries = malloc(entry_count * ch->entry_size);
    if (!ch->entries) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    // Read Ci
    if (fread(ch->entries, ch->entry_size, entry_count, f) != entry_count) {
        free(ch->entries);
        ch->entries = NULL;
        fclose(f);
        return ESP_FAIL;
    }
    
    // Allocate LAi
    ch->lai_indices = malloc(lai_count * sizeof(uint32_t));
    if (!ch->lai_indices) {
        free(ch->entries);
        ch->entries = NULL;
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    // Read LAi
    if (fread(ch->lai_indices, sizeof(uint32_t), lai_count, f) != lai_count) {
        free(ch->entries);
        free(ch->lai_indices);
        ch->entries = NULL;
        ch->lai_indices = NULL;
        fclose(f);
        return ESP_FAIL;
    }
    
    ch->entry_count = entry_count;
    ch->lai_count = lai_count;
    
    fclose(f);
    return ESP_OK;
}

// Add entry to LAi
esp_err_t ps_lai_add(ps_channel_state_t *ch, uint32_t ci_index) {
    // Check if already present
    for (uint32_t i = 0; i < ch->lai_count; i++) {
        if (ch->lai_indices[i] == ci_index) {
            return ESP_OK;  // Already present
        }
    }
    
    // Grow LAi array if needed
    if (ch->lai_count >= MAX_ENTRIES) {
        return ESP_ERR_NO_MEM;
    }
    
    ch->lai_indices[ch->lai_count++] = ci_index;
    
    // Persist to disk
    return ps_save_channel_cache(ch);
}

// Remove entry from LAi
esp_err_t ps_lai_remove(ps_channel_state_t *ch, uint32_t ci_index) {
    for (uint32_t i = 0; i < ch->lai_count; i++) {
        if (ch->lai_indices[i] == ci_index) {
            // Shift remaining entries
            memmove(&ch->lai_indices[i], 
                    &ch->lai_indices[i + 1],
                    (ch->lai_count - i - 1) * sizeof(uint32_t));
            ch->lai_count--;
            
            // Persist to disk
            return ps_save_channel_cache(ch);
        }
    }
    return ESP_ERR_NOT_FOUND;
}
```

**Pain Point**: Atomic file operations on SD card. Use temp file + rename pattern.

### Migration Strategy

**Old Cache Format**: Just Ci, no header
```
[entry[0], entry[1], ..., entry[N-1]]
```

**New Cache Format**: Header + Ci + LAi
```
[entry_count, lai_count] [entry[0], ..., entry[N-1]] [lai[0], ..., lai[M-1]]
```

**Backward Compatibility**:
```c
esp_err_t ps_load_channel_cache(ps_channel_state_t *ch) {
    struct stat st;
    if (stat(ch->cache_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Try to read header
    FILE *f = fopen(ch->cache_path, "rb");
    uint32_t header[2];
    if (fread(header, sizeof(uint32_t), 2, f) != 2) {
        fclose(f);
        return ESP_FAIL;
    }
    
    // Heuristic: If header looks valid, it's new format
    if (header[0] < MAX_ENTRIES && header[1] <= header[0]) {
        // New format - continue reading
        return load_new_format(f, ch, header[0], header[1]);
    }
    
    // Old format - rebuild LAi from scratch
    fseek(f, 0, SEEK_SET);
    esp_err_t err = load_old_format(f, ch);
    fclose(f);
    
    if (err == ESP_OK) {
        // Rebuild LAi by scanning files
        rebuild_lai_from_filesystem(ch);
        
        // Save in new format
        ps_save_channel_cache(ch);
    }
    
    return err;
}
```

## Phase 2: Add Connectivity State Tracking

### Goal
Centralize connectivity state in `p3a_state.c` with clear online/offline mode flag.

### Files to Modify

#### 1. `components/p3a_core/include/p3a_state.h`

**Add**:
```c
// Connectivity state
typedef struct {
    bool wifi_connected;
    bool internet_available;
    bool mpx_registered;
    bool mqtt_connected;
    
    // Derived
    bool online_mode;  // All of the above
    
    // Timestamps for retry logic
    uint32_t wifi_last_disconnect_ms;
    uint32_t internet_last_check_ms;
    uint32_t mqtt_last_disconnect_ms;
} p3a_connectivity_state_t;

// Connectivity APIs
bool p3a_connectivity_is_online(void);
bool p3a_connectivity_has_wifi(void);
bool p3a_connectivity_has_internet(void);
bool p3a_connectivity_is_registered(void);
bool p3a_connectivity_has_mqtt(void);

// State setters (called by component callbacks)
void p3a_connectivity_set_wifi(bool connected);
void p3a_connectivity_set_internet(bool available);
void p3a_connectivity_set_mpx_registered(bool registered);
void p3a_connectivity_set_mqtt(bool connected);

// User-facing status
const char* p3a_connectivity_get_status_message(void);

// Callbacks for online/offline transitions
typedef void (*p3a_online_mode_cb_t)(bool online, void *user_data);
esp_err_t p3a_connectivity_register_callback(p3a_online_mode_cb_t cb, void *user_data);
```

#### 2. `components/p3a_core/p3a_state.c`

**Add to internal state**:
```c
static struct {
    // ... existing fields ...
    
    p3a_connectivity_state_t connectivity;
    
    // Callbacks
    p3a_online_mode_cb_t online_callbacks[8];
    void *online_callback_data[8];
    int online_callback_count;
} s_state;

void p3a_connectivity_set_wifi(bool connected) {
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    bool was_online = s_state.connectivity.online_mode;
    s_state.connectivity.wifi_connected = connected;
    
    if (!connected) {
        // Cascading: WiFi down means everything down
        s_state.connectivity.internet_available = false;
        s_state.connectivity.mqtt_connected = false;
    } else {
        // WiFi up - trigger internet check
        check_internet_connectivity_async();
    }
    
    // Recompute online_mode
    s_state.connectivity.online_mode = 
        s_state.connectivity.wifi_connected &&
        s_state.connectivity.internet_available &&
        s_state.connectivity.mpx_registered &&
        s_state.connectivity.mqtt_connected;
    
    bool is_online = s_state.connectivity.online_mode;
    
    xSemaphoreGive(s_state.mutex);
    
    // Notify callbacks if online mode changed
    if (was_online != is_online) {
        for (int i = 0; i < s_state.online_callback_count; i++) {
            if (s_state.online_callbacks[i]) {
                s_state.online_callbacks[i](is_online, s_state.online_callback_data[i]);
            }
        }
    }
}
```

#### 3. `components/wifi_manager/app_wifi.c`

**Add callback to p3a_state**:
```c
static void on_wifi_connected_cb(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
    ESP_LOGI(TAG, "WiFi connected");
    
    // Notify p3a_state
    p3a_connectivity_set_wifi(true);
}

static void on_wifi_disconnected_cb(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
    ESP_LOGI(TAG, "WiFi disconnected");
    
    // Notify p3a_state
    p3a_connectivity_set_wifi(false);
}
```

#### 4. `components/makapix/makapix_mqtt.c`

**Add callback to p3a_state**:
```c
static void on_mqtt_connected(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data) {
    ESP_LOGI(TAG, "MQTT connected");
    
    // Notify p3a_state
    p3a_connectivity_set_mqtt(true);
}

static void on_mqtt_disconnected(void *handler_args, esp_event_base_t base,
                                  int32_t event_id, void *event_data) {
    ESP_LOGI(TAG, "MQTT disconnected");
    
    // Notify p3a_state
    p3a_connectivity_set_mqtt(false);
}
```

**Pain Point**: Internet connectivity check requires new HTTP client code. Use simple HTTP HEAD request to reliable endpoint.

## Phase 3: Update Play Scheduler to Use LAi

### Goal
Modify `play_scheduler_pick.c` to pick from LAi instead of scanning Ci.

### Files to Modify

#### 1. `components/play_scheduler/play_scheduler_pick.c`

**Replace**:
```c
// OLD: Scan through Ci with retries
bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out) {
    size_t ch_idx = ps_swrr_select_channel(state);
    ps_channel_state_t *ch = &state->channels[ch_idx];
    
    // Retry loop (problematic)
    for (int retry = 0; retry < ch->entry_count; retry++) {
        uint32_t idx = (ch->cursor + retry) % ch->entry_count;
        // ... check file_exists() ...
    }
}
```

**With**:
```c
// NEW: Pick directly from LAi (no retries)
bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out) {
    size_t ch_idx = ps_swrr_select_channel(state);
    ps_channel_state_t *ch = &state->channels[ch_idx];
    
    if (ch->lai_count == 0) {
        return false;  // No available entries
    }
    
    // Pick from LAi based on mode
    uint32_t lai_idx;
    if (state->pick_mode == PS_PICK_RECENCY) {
        lai_idx = ch->lai_cursor % ch->lai_count;
        ch->lai_cursor++;
    } else {  // PS_PICK_RANDOM
        uint32_t rand = ps_prng_next(&ch->pick_rng_state);
        lai_idx = rand % ch->lai_count;
    }
    
    // Get Ci index from LAi
    uint32_t ci_idx = ch->lai_indices[lai_idx];
    channel_entry_t *entry = &ch->entries[ci_idx];
    
    // Build artwork (file guaranteed to exist)
    return build_artwork_from_entry(entry, out);
}
```

#### 2. `components/play_scheduler/play_scheduler_swrr.c`

**Update weight calculation**:
```c
void ps_swrr_calculate_weights(ps_state_t *state) {
    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        
        // Weight based on LAi, not Ci
        if (ch->lai_count == 0) {
            ch->weight = 0;  // Skip channels with no available content
        } else if (state->exposure_mode == PS_EXPOSURE_EQUAL) {
            ch->weight = 1000;  // Equal weight
        } else {
            // Proportional to lai_count
            ch->weight = ch->lai_count;
        }
    }
}
```

**Pain Point**: Need to handle empty LAi gracefully. Channels with `lai_count==0` should be skipped by SWRR.

## Phase 4: Integrate Download Manager with LAi

### Goal
Update download manager to add entries to LAi upon completion.

### Files to Modify

#### 1. `components/channel_manager/download_manager.c`

**Add to download completion**:
```c
static void on_download_complete(const download_request_t *req) {
    ESP_LOGI(TAG, "Download complete: %s", req->storage_key);
    
    // Find ci_index for this storage_key
    uint32_t ci_index = find_ci_index_by_storage_key(req->channel_id, req->storage_key);
    if (ci_index == UINT32_MAX) {
        ESP_LOGW(TAG, "Entry not found in Ci after download");
        return;
    }
    
    // Add to LAi
    esp_err_t err = ps_lai_add_by_channel_id(req->channel_id, ci_index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add to LAi: %s", esp_err_to_name(err));
        return;
    }
    
    // Check if this was the first entry
    uint32_t lai_count = ps_get_lai_count(req->channel_id);
    if (lai_count == 1) {
        // First entry - trigger playback if not already playing
        trigger_playback_if_needed();
    }
    
    // Signal download manager to continue
    download_manager_signal_work_available();
}
```

**Pain Point**: Need cross-module API for Play Scheduler to expose LAi add/remove functions.

## Phase 5: Update Channel Refresh to Build LAi

### Goal
When channel refresh completes, build initial LAi by checking file existence.

### Files to Modify

#### 1. `components/play_scheduler/play_scheduler_refresh.c`

**Add to refresh completion**:
```c
static void on_refresh_complete(const char *channel_id, 
                                 channel_entry_t *entries,
                                 uint32_t entry_count) {
    ESP_LOGI(TAG, "Refresh complete: %s (%lu entries)", 
             channel_id, (unsigned long)entry_count);
    
    ps_channel_state_t *ch = find_channel(channel_id);
    if (!ch) return;
    
    // Replace Ci
    free(ch->entries);
    ch->entries = entries;
    ch->entry_count = entry_count;
    
    // Build LAi by checking which files exist
    ch->lai_count = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        char filepath[256];
        build_vault_filepath(&entries[i], filepath, sizeof(filepath));
        
        if (file_exists(filepath) && !has_404_marker(filepath)) {
            ch->lai_indices[ch->lai_count++] = i;
        }
    }
    
    ESP_LOGI(TAG, "Channel %s: lai_count=%lu/%lu", 
             channel_id, (unsigned long)ch->lai_count, 
             (unsigned long)entry_count);
    
    // Save to disk (Ci + LAi)
    ps_save_channel_cache(ch);
    
    // Signal download manager (new entries may need downloading)
    download_manager_signal_work_available();
}
```

**Pain Point**: LAi rebuild involves stat() on every entry. Can be slow for large channels (1000+ entries). Consider doing this on background task.

## Phase 6: Add User Messaging

### Goal
Display informative messages based on connectivity and channel state.

### Files to Modify

#### 1. `main/display_renderer.c`

**Add message rendering**:
```c
void display_renderer_show_message(const char *title, const char *body) {
    // Clear screen
    memset(s_framebuffer, 0, FRAMEBUFFER_SIZE);
    
    // Draw title (large font)
    draw_text_centered(title, SCREEN_WIDTH/2, 200, FONT_LARGE);
    
    // Draw body (medium font, multi-line)
    draw_text_multiline(body, 50, 300, SCREEN_WIDTH-100, FONT_MEDIUM);
    
    // Swap buffers
    swap_buffers();
}
```

#### 2. `main/animation_player.c`

**Check for messages before rendering**:
```c
void animation_player_render_frame(void) {
    // Check if we should show a message instead
    char detail[256];
    const char *msg = p3a_get_user_message(detail, sizeof(detail));
    
    if (msg) {
        display_renderer_show_message("p3a", msg);
        return;
    }
    
    // Normal frame rendering
    // ...
}
```

#### 3. `components/p3a_core/p3a_state.c`

**Add message logic**:
```c
const char* p3a_get_user_message(char *detail_buf, size_t detail_len) {
    // Priority 1: Connectivity
    if (!p3a_connectivity_has_wifi()) {
        return "No Wi-Fi\n\nLong-press to configure";
    }
    if (!p3a_connectivity_has_internet()) {
        return "No Internet\n\nCheck your router";
    }
    if (!p3a_connectivity_is_registered()) {
        return "Not Registered\n\nLong-press to register";
    }
    if (!p3a_connectivity_has_mqtt()) {
        return "Connecting to Makapix...";
    }
    
    // Priority 2: Channel state (needs Play Scheduler query)
    // ... (see 05-USER-FEEDBACK.md for full logic)
    
    return NULL;  // No message, play normally
}
```

**Pain Point**: Message logic needs to query Play Scheduler state without heavy operations. Need clean API.

## Phase 7: Update Web UI Status Endpoint

### Goal
Populate `/api/status` with lightweight in-memory data, no file scanning.

### Files to Modify

#### 1. `components/http_api/http_api_rest.c`

**Update status handler**:
```c
static esp_err_t handle_status_get(httpd_req_t *req) {
    // All data from in-memory state
    cJSON *root = cJSON_CreateObject();
    
    // Connectivity
    cJSON *conn = cJSON_CreateObject();
    cJSON_AddBoolToObject(conn, "wifi", p3a_connectivity_has_wifi());
    cJSON_AddBoolToObject(conn, "internet", p3a_connectivity_has_internet());
    cJSON_AddBoolToObject(conn, "registered", p3a_connectivity_is_registered());
    cJSON_AddBoolToObject(conn, "mqtt", p3a_connectivity_has_mqtt());
    cJSON_AddBoolToObject(conn, "online_mode", p3a_connectivity_is_online());
    cJSON_AddItemToObject(root, "connectivity", conn);
    
    // Playback (from Play Scheduler)
    ps_stats_t stats;
    if (play_scheduler_get_stats(&stats) == ESP_OK) {
        cJSON *playback = cJSON_CreateObject();
        cJSON_AddStringToObject(playback, "current_channel", stats.current_channel_id);
        // ... more fields ...
        cJSON_AddItemToObject(root, "playback", playback);
    }
    
    // Channels (in-memory stats only)
    cJSON *channels = cJSON_CreateArray();
    for (size_t i = 0; i < MAX_CHANNELS; i++) {
        // Read from Play Scheduler state
        ps_channel_state_t *ch = ps_get_channel_state(i);
        if (!ch || !ch->active) continue;
        
        cJSON *ch_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(ch_obj, "channel_id", ch->channel_id);
        cJSON_AddNumberToObject(ch_obj, "entry_count", ch->entry_count);
        cJSON_AddNumberToObject(ch_obj, "lai_count", ch->lai_count);  // NEW
        cJSON_AddBoolToObject(ch_obj, "refreshing", ch->refresh_in_progress);
        cJSON_AddItemToArray(channels, ch_obj);
    }
    cJSON_AddItemToObject(root, "channels", channels);
    
    // Serialize and send
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}
```

**Pain Point**: Need to expose Play Scheduler state without breaking encapsulation. Add getter functions.

## Migration Pain Points Summary

### 1. Cache File Format Migration
**Issue**: Old caches don't have LAi section.
**Solution**: Detect old format, rebuild LAi from filesystem, save in new format.
**Risk**: Slow on first boot after upgrade (1000 stat() calls).

### 2. SD Card Atomic Writes
**Issue**: Power loss during cache write could corrupt file.
**Solution**: Write to `.tmp` file, atomic rename.
**Risk**: Filesystem may not guarantee atomicity.

### 3. Cross-Module Dependencies
**Issue**: Download Manager needs to call Play Scheduler LAi functions.
**Solution**: Expose clean API in `play_scheduler.h`:
```c
esp_err_t play_scheduler_lai_add(const char *channel_id, uint32_t ci_index);
esp_err_t play_scheduler_lai_remove(const char *channel_id, uint32_t ci_index);
```
**Risk**: Circular dependencies if not careful.

### 4. Thread Safety
**Issue**: Multiple tasks accessing channel state (refresh, download, playback).
**Solution**: Use existing `s_state.mutex` consistently.
**Risk**: Deadlocks if mutex is held during blocking operations (HTTP, file I/O).

### 5. Internet Connectivity Check
**Issue**: No existing code for checking internet availability.
**Solution**: Add simple HTTP HEAD request:
```c
bool check_internet_connectivity(void) {
    esp_http_client_config_t config = {
        .url = "http://clients3.google.com/generate_204",
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    return (err == ESP_OK && status == 204);
}
```
**Risk**: Network calls can be slow, must be non-blocking.

### 6. Testing Coverage
**Issue**: Complex state machine is hard to test.
**Solution**: Add unit tests for:
- LAi operations (add, remove, contains)
- Connectivity state transitions
- Message priority logic
- SWRR with mixed lai_counts

**Risk**: Insufficient test coverage leads to regression bugs.

## Rollout Strategy

### Phase 1: Dark Launch (Week 1)
- Implement LAi support but don't use it yet
- Run both old and new code paths in parallel
- Log discrepancies for debugging

### Phase 2: Canary (Week 2)
- Enable LAi on single test device
- Monitor for crashes, performance issues
- Collect logs for analysis

### Phase 3: Beta (Week 3)
- Enable for beta testers (10 devices)
- Gather user feedback on messaging
- Fix bugs discovered in field

### Phase 4: Production (Week 4)
- Roll out to all devices via OTA
- Monitor crash rates, metrics
- Prepare hotfix if needed

## Success Metrics

### Performance
- [ ] Playback latency < 100ms (target: 66ms)
- [ ] LAi rebuild on boot < 5 seconds for 1000 entries
- [ ] No frame drops during concurrent download

### Reliability
- [ ] No crashes in 7-day test
- [ ] Graceful degradation on network loss
- [ ] Correct LAi state after power cycle

### User Experience
- [ ] Clear messages for all states
- [ ] No "stuck" states requiring reboot
- [ ] Smooth transitions (no flashing)

## Rollback Plan

If critical issues are discovered:

1. **Immediate**: Revert to previous firmware via OTA
2. **Short-term**: Fix bugs in hotfix branch
3. **Long-term**: Re-test with improved coverage

**Rollback Trigger**: Crash rate > 5% within 24h of deployment.

## Documentation Updates

- [ ] Update `docs/INFRASTRUCTURE.md` with LAi architecture
- [ ] Add `docs/STATE-MACHINE.md` (this document set)
- [ ] Update REST API documentation for `/api/status` changes
- [ ] Add troubleshooting guide for connectivity issues
- [ ] Update contributor guide with state machine overview

## Timeline

| Phase | Duration | Dependencies |
|-------|----------|-------------|
| LAi Support | 2 days | None |
| Connectivity State | 1 day | LAi Support |
| Play Scheduler Integration | 2 days | LAi Support, Connectivity |
| Download Manager | 1 day | Play Scheduler Integration |
| User Messaging | 1 day | All above |
| Web UI | 1 day | All above |
| Testing | 2 days | All above |
| **Total** | **10 days** | |

Add 30% buffer for unexpected issues: **13 days total**.

## Open Questions Requiring Decisions

See `07-OPEN-QUESTIONS.md` for detailed analysis of:
- Internet connectivity check endpoint
- MQTT retry backoff strategy
- LAi rebuild frequency
- Download bandwidth management
- Message display behavior

## Next Steps

1. Review this implementation plan
2. Answer open questions in `07-OPEN-QUESTIONS.md`
3. Create GitHub issues for each phase
4. Assign owners and start implementation
5. Set up CI/CD for automated testing
