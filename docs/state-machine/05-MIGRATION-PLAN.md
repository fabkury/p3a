# p3a State Machine — Migration Plan

**Document:** 05-MIGRATION-PLAN.md  
**Status:** Final Plan

---

## 1. Migration Strategy Overview

The migration will be implemented in **6 phases**, each building on the previous:

```
Phase 1: LAi Data Structure + LTF System
    └─► Phase 2: LAi Maintenance + Robustness
            └─► Phase 3: Play Scheduler Integration
                    └─► Phase 4: Connectivity State
                            └─► Phase 5: Early Downloads + Event-Driven Architecture
                                    └─► Phase 6: Web UI Optimization
```

Each phase should be:
- Independently testable
- Backward compatible where possible
- Safe to deploy incrementally

---

## 2. Phase 1: LAi Data Structure + LTF System

**Goal**: Establish new cache file format with LAi and Load Tracker File system

### 2.1 Tasks

1. **Define new header structure** in `components/channel_manager/include/channel_cache.h`:
   ```c
   typedef struct __attribute__((packed)) {
       uint32_t magic;           // 0x50334143 ('P3AC')
       uint16_t version;         // 1
       uint16_t flags;
       uint32_t ci_count;
       uint32_t lai_count;
       uint32_t ci_offset;
       uint32_t lai_offset;
       uint32_t checksum;
       uint8_t reserved[16];
   } channel_cache_header_t;
   ```

2. **Implement save function** `channel_cache_save()`:
   - Write header
   - Write Ci entries
   - Write LAi indices
   - Compute checksum
   - Atomic rename

3. **Implement load function** `channel_cache_load()`:
   - Check magic
   - Handle old format (migration path)
   - Load Ci and LAi arrays
   - Verify checksum

4. **Implement migration** `channel_cache_migrate_legacy()`:
   - Detect old format (no magic header)
   - Load as raw Ci entries
   - Rebuild LAi by scanning files
   - Save in new format

5. **Implement LTF system** in `components/channel_manager/load_tracker.c`:
   - `ltf_can_download()` - check if download is allowed
   - `ltf_record_failure()` - record failed load attempt
   - `ltf_clear()` - clear LTF on successful load
   - LTF file format: simple JSON or binary with attempt count

### 2.2 Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `channel_cache.h` | Create | Header structures and API |
| `channel_cache.c` | Create | Save/load/migrate implementation |
| `load_tracker.h` | Create | LTF API |
| `load_tracker.c` | Create | LTF implementation |
| `play_scheduler.c` | Modify | Use new load function |

### 2.3 Estimated Effort

3-4 days

---

## 3. Phase 2: LAi Maintenance + Robustness

**Goal**: Keep LAi in sync with file system state; handle failures gracefully

### 3.1 Tasks

1. **Implement `lai_add_entry()`**:
   - Called when download completes
   - Check if entry already in LAi
   - Append to LAi array
   - Mark cache dirty

2. **Implement `lai_remove_entry()`**:
   - Called when file evicted or load fails
   - Find and remove from LAi array (swap with last)
   - Mark cache dirty

3. **Implement `lai_rebuild()`**:
   - Iterate all Ci entries
   - Check file existence + LTF status
   - Build fresh LAi
   - Used on migration or suspected corruption

4. **Implement dirty tracking and persistence**:
   - Set dirty flag on LAi change
   - **15-second debounce** timer
   - Persist on timer expiry

5. **Implement load failure handler**:
   - On file load failure, record in LTF
   - Delete corrupted file
   - Remove from LAi
   - Signal to try another artwork

### 3.2 Integration Points

```c
// In download_manager.c, after successful download:
void on_download_complete(const char *channel_id, 
                          const char *storage_key,
                          uint32_t ci_index) {
    // Clear any LTF (success proves file is good)
    ltf_clear(storage_key);
    
    // Add to LAi
    channel_cache_t *cache = get_channel_cache(channel_id);
    lai_add_entry(cache, ci_index);
}

// In play_scheduler.c, when file fails to load:
void on_load_failed(const char *storage_key, const char *reason) {
    // Record failure in LTF
    ltf_record_failure(storage_key, reason);
    
    // Delete file
    unlink(build_path(storage_key));
    
    // Remove from LAi
    channel_cache_t *cache = find_cache_for_storage_key(storage_key);
    uint32_t ci_idx = find_ci_index(cache, storage_key);
    lai_remove_entry(cache, ci_idx);
}
```

### 3.3 Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `channel_cache.c` | Modify | Add lai_add/remove/rebuild |
| `download_manager.c` | Modify | Call lai_add_entry on success, clear LTF |
| `play_scheduler.c` | Modify | Add load failure handler |

### 3.4 Estimated Effort

3-4 days

---

## 4. Phase 3: Play Scheduler Integration

**Goal**: PS picks from LAi instead of checking file existence

### 4.1 Tasks

1. **Modify `ps_load_channel_cache()`**:
   - Load both Ci and LAi
   - Set `available_count` from LAi
   - Set `active = (available_count > 0)`

2. **Modify `ps_pick_next_available()`**:
   - Remove `file_exists()` checks
   - Pick directly from LAi indices
   - Random mode: random index in LAi
   - Sequential mode: cursor through LAi

3. **Modify SWRR weight calculation**:
   - Use `available_count` for active/inactive determination
   - Equal weights for all active channels (LAi size not considered)
   - Exclude channels with zero availability

4. **Update `ps_channel_state_t`**:
   - Add `uint32_t *available_indices`
   - Add `size_t available_count`
   - Remove inline file checking logic

5. **Implement zero-to-one transition**:
   - When LAi transitions from empty to non-empty during "waiting" state
   - Immediately trigger playback

6. **Implement load failure recovery**:
   - Call `on_load_failed()` when animation player reports failure
   - Try picking another artwork
   - Fall back to "waiting" message if LAi becomes empty

### 4.2 Key Code Changes

```c
// BEFORE:
static bool is_entry_available(ps_channel_state_t *ch, uint32_t ci_idx) {
    char filepath[256];
    build_vault_filepath(&ch->entries[ci_idx], filepath);
    return file_exists(filepath);
}

// AFTER:
static bool pick_from_lai(ps_channel_state_t *ch, 
                          uint32_t *out_ci_idx,
                          ps_pick_mode_t mode) {
    if (ch->available_count == 0) return false;
    
    uint32_t lai_idx;
    if (mode == PS_PICK_RANDOM) {
        lai_idx = prng_next() % ch->available_count;
    } else {
        lai_idx = ch->cursor++ % ch->available_count;
    }
    
    *out_ci_idx = ch->available_indices[lai_idx];
    return true;
}
```

### 4.3 Files to Modify

| File | Action | Description |
|------|--------|-------------|
| `play_scheduler.c` | Major | Load LAi, use for picks |
| `play_scheduler_pick.c` | Major | Remove file_exists checks |
| `play_scheduler_swrr.c` | Modify | Use available_count |
| `play_scheduler_internal.h` | Modify | Update state structure |

### 4.4 Estimated Effort

3-4 days

---

## 5. Phase 4: Connectivity State Machine

**Goal**: Hierarchical connectivity states for clear user communication

### 5.1 Tasks

1. **Create `connectivity_state.c/.h`**:
   - Define state enum (NO_WIFI, NO_INTERNET, NO_REGISTRATION, NO_MQTT, ONLINE)
   - Implement state transitions
   - Implement internet check (DNS lookup for example.com)

2. **Integrate with existing event system**:
   - Hook into WiFi connect/disconnect events
   - Hook into MQTT connect/disconnect events
   - Trigger internet check on WiFi connect

3. **Implement internet check**:
   - DNS lookup for `example.com`
   - Retry every 60 seconds in NO_INTERNET state

4. **Implement MQTT reconnection**:
   - Exponential backoff with jitter
   - 5s → 10s → 20s → 40s → 80s → 160s → 300s (cap)

5. **Update user-facing messages**:
   - `p3a_state` uses connectivity_state for channel messages
   - Web UI displays appropriate connectivity status

### 5.2 Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `connectivity_state.h` | Create | State definitions and API |
| `connectivity_state.c` | Create | State machine implementation |
| `p3a_state.c` | Modify | Use connectivity state for messages |
| `app_wifi.c` | Modify | Signal WiFi events |
| `makapix_connection.c` | Modify | Signal MQTT events |

### 5.3 Estimated Effort

2-3 days

---

## 6. Phase 5: Early Downloads + Event-Driven Architecture

**Goal**: Start downloads before channel refresh completes; migrate to message queues

### 6.1 Tasks

1. **Modify refresh task**:
   - Signal download manager after each batch
   - Don't wait for full refresh to signal

2. **Modify download manager**:
   - Remove `wait_for_refresh_done()` blocking
   - Start scanning Ci immediately when entries available
   - Use round-robin across channels for download selection
   - Check LTF before downloading

3. **Migrate to event-driven architecture**:
   - Replace polling loops with FreeRTOS queues
   - Define message types for download requests, completions, evictions
   - Clean abort handling via queue messages

4. **Implement refresh scheduling**:
   - Refresh all channels on SC reception
   - Start 2-hour timer when refresh round completes
   - Cancel timer on new SC

### 6.2 Key Code Changes

```c
// BEFORE (refresh task):
esp_err_t refresh_channel(const char *channel_id) {
    while (has_more_pages) {
        query_page(&entries, &count);
        append_to_ci(entries, count);
    }
    // Signal only after complete
    makapix_channel_signal_refresh_done();
}

// AFTER:
esp_err_t refresh_channel(const char *channel_id) {
    while (has_more_pages && !abort_requested()) {
        query_page(&entries, &count);
        append_to_ci(entries, count);
        
        // Signal after each batch
        download_manager_signal_work_available();
    }
    makapix_channel_signal_refresh_done();
}

// Download manager - event driven:
void download_task(void *arg) {
    while (true) {
        download_msg_t msg;
        xQueueReceive(s_download_queue, &msg, portMAX_DELAY);
        
        switch (msg.type) {
            case DL_MSG_WORK_AVAILABLE:
                process_pending_downloads();
                break;
            case DL_MSG_ABORT:
                abort_current_download();
                break;
            // ...
        }
    }
}
```

### 6.3 Files to Modify

| File | Action | Description |
|------|--------|-------------|
| `play_scheduler_refresh.c` | Modify | Signal per-batch |
| `download_manager.c` | Major | Event-driven, early start, round-robin |
| `makapix_channel_refresh.c` | Modify | Signal per-batch |

### 6.4 Estimated Effort

3-4 days

---

## 7. Phase 6: Web UI Optimization

**Goal**: Instant Web UI with cached LAi statistics

### 7.1 Tasks

1. **Modify `/channels/stats`**:
   - Return LAi count (available) and Ci count (total)
   - No file scanning required

2. **Cache stats in memory**:
   - Update on LAi change
   - Return cached values

3. **Add connectivity status to API**:
   - `/api/state` returns connectivity_state
   - Web UI displays connectivity clearly

4. **Implement UI polling**:
   - Poll every 5 seconds when tab is active
   - Pause polling when tab is inactive

### 7.2 Key Code Changes

```c
// BEFORE:
esp_err_t h_get_channels_stats(httpd_req_t *req) {
    // Scans files for each channel - SLOW
    makapix_channel_count_cached("all", &total, &cached);
    // ...
}

// AFTER:
esp_err_t h_get_channels_stats(httpd_req_t *req) {
    // Read from memory - FAST
    channel_cache_t *all_cache = get_channel_cache("all");
    size_t all_total = all_cache ? all_cache->entry_count : 0;
    size_t all_available = all_cache ? all_cache->available_count : 0;
    // ...
}
```

### 7.3 Files to Modify

| File | Action | Description |
|------|--------|-------------|
| `http_api_rest.c` | Modify | Return cached stats |
| `webui/*.html` | Modify | Display connectivity status, handle polling |

### 7.4 Estimated Effort

1-2 days

---

## 8. Total Estimated Effort

| Phase | Days | Cumulative |
|-------|------|------------|
| Phase 1: Data Structure + LTF | 3-4 | 3-4 |
| Phase 2: LAi Maintenance | 3-4 | 6-8 |
| Phase 3: PS Integration | 3-4 | 9-12 |
| Phase 4: Connectivity | 2-3 | 11-15 |
| Phase 5: Early Downloads + Events | 3-4 | 14-19 |
| Phase 6: Web UI | 1-2 | 15-21 |

**Total: 15-21 engineering days**

---

## 9. Risk Mitigation

### 9.1 Data Loss Risk

**Risk**: Corrupted cache file loses all LAi data.

**Mitigation**:
- Atomic writes with tmp+rename
- Checksum validation
- Auto-rebuild LAi on corruption

### 9.2 Infinite Loop Risk

**Risk**: Corrupted file causes infinite download loop.

**Mitigation**:
- LTF system tracks failed loads
- After 3 failures, file marked terminal
- Terminal files never re-downloaded

### 9.3 Race Condition Risk

**Risk**: Download completes while Play Scheduler is picking.

**Mitigation**:
- Mutex protection on LAi access
- Copy-on-read for PS picks
- LAi changes signaled via event

### 9.4 All-Files-Deleted Edge Case

**Risk**: All files deleted from SD card; LAi points to nothing.

**Mitigation**:
- Each failed load removes entry from LAi
- Eventually LAi empties naturally
- System falls back to "waiting for downloads" or "no artworks"
- Optional: quick validation on startup (sample 10 files)

### 9.5 Backward Compatibility Risk

**Risk**: Old firmware can't read new cache format.

**Mitigation**:
- New format clearly identified by magic header
- Old firmware will treat as missing cache and rebuild
- One-way upgrade only (acceptable)

### 9.6 Performance Regression Risk

**Risk**: LAi maintenance adds overhead.

**Mitigation**:
- LAi operations are O(1) append / O(n) remove (n=available)
- 15-second debounced persistence
- Profile before/after

---

*Previous: [03-PLAY-SCHEDULER-BEHAVIOR.md](03-PLAY-SCHEDULER-BEHAVIOR.md)*  
*Next: [06-PAIN-POINTS.md](06-PAIN-POINTS.md)*
