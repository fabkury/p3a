# p3a State Machine — Current Pain Points & Architectural Analysis

**Document:** 06-PAIN-POINTS.md  
**Status:** Analysis (Reference)

---

## 1. Overview

This document catalogs the current inefficiencies and architectural weaknesses that the refactoring addresses.

---

## 2. Critical Pain Points

### 2.1 On-the-Fly File Existence Checks

**Location**: `play_scheduler_pick.c`, `download_manager.c`

**Current Behavior**:
```c
// Every pick attempt does this:
static bool is_entry_available(const makapix_channel_entry_t *entry) {
    char filepath[256];
    build_vault_filepath(entry, filepath, sizeof(filepath));
    return file_exists(filepath) && !has_404_marker(filepath);  // Two stat() calls!
}
```

**Problems**:
- **I/O during playback**: Each pick attempt may check 5+ files
- **Random mode penalty**: Random pick may retry many times
- **SDIO bus contention**: Competes with ongoing downloads
- **No availability count**: Can't know how many files are available without full scan

**Impact**: High — Causes playback stutters and slow channel switching

**Solution**: LAi (Phase 2-3)

---

### 2.2 Web UI File Scanning

**Location**: `http_api_rest.c`, function `h_get_channels_stats()`

**Current Behavior**:
```c
// On each /channels/stats request:
makapix_channel_count_cached("all", channel_path, vault_path, &total, &cached);

// Which does this for each channel:
for (size_t i = 0; i < entry_count; i++) {
    char filepath[256];
    build_vault_filepath(&entries[i], filepath);
    if (file_exists(filepath)) cached++;  // stat() for every entry!
}
```

**Problems**:
- **~5 seconds for 1000 entries**: Completely blocks HTTP response
- **Repeated on each UI load**: No caching between requests
- **Blocks during OTA check**: User thinks device is frozen
- **SDIO contention**: Competes with playback

**Impact**: High — Poor Web UI responsiveness

**Solution**: Return LAi count directly (Phase 6)

---

### 2.3 Blocking on Full Refresh Before Download

**Location**: `download_manager.c`, function `download_task()`

**Current Behavior**:
```c
// Download task waits for full refresh:
if (!makapix_channel_is_refresh_done()) {
    ESP_LOGI(TAG, "Waiting for channel refresh...");
    makapix_channel_wait_for_refresh(portMAX_DELAY);  // Blocks!
}
```

**Problems**:
- **Large channel delays**: 1000+ entry channels take minutes to refresh
- **No early playback**: User sees "Loading..." for too long
- **Wasted time**: Could be downloading while refresh continues

**Impact**: High — Poor cold-start experience

**Solution**: Start downloads after first batch (Phase 5)

---

### 2.4 Flat Connectivity State Model

**Location**: `makapix_channel_events.c`

**Current Behavior**:
```c
// Independent event bits:
#define MAKAPIX_EVENT_WIFI_CONNECTED     (1 << 0)
#define MAKAPIX_EVENT_WIFI_DISCONNECTED  (1 << 1)
#define MAKAPIX_EVENT_MQTT_CONNECTED     (1 << 2)
#define MAKAPIX_EVENT_MQTT_DISCONNECTED  (1 << 3)
```

**Problems**:
- **No hierarchy**: WiFi + MQTT treated independently
- **Confusing messages**: May say "No MQTT" when WiFi is down
- **No internet check**: WiFi connected doesn't mean internet available
- **State explosion**: 2^4 = 16 possible combinations

**Impact**: Medium — Confusing user experience

**Solution**: Hierarchical state machine (Phase 4)

---

## 3. Medium Pain Points

### 3.1 No Ci/LAi Atomicity

**Location**: `play_scheduler.c`, `ps_load_channel_cache()`

**Current Behavior**:
```c
// Ci is loaded from .bin file
// LAi doesn't exist - computed on-the-fly
// No guarantee of consistency
```

**Problems**:
- **Race conditions**: File could be evicted between Ci load and pick
- **No crash recovery**: Partial state after crash
- **No version tracking**: Can't detect format changes

**Impact**: Medium — Potential for playing missing files

**Solution**: Unified cache file with header (Phase 1)

---

### 3.2 Download Manager Polling Pattern

**Location**: `download_manager.c`

**Current Behavior**:
```c
while (true) {
    // Wait for signal
    makapix_channel_wait_for_downloads_needed(portMAX_DELAY);
    
    // Get next download
    esp_err_t err = dl_get_next_download(&req, &snapshot);
    if (err == ESP_ERR_NOT_FOUND) {
        // All done, go back to waiting
        continue;
    }
    // ... download ...
}
```

**Problems**:
- **Redundant scanning**: Scans entire cache file on each wake
- **No cursor persistence**: Restarts from beginning after each download
- **Event loss**: If signal fires during download, may miss it

**Impact**: Medium — Inefficient but functional

**Solution**: Event-driven architecture with message queues (Phase 5)

---

### 3.3 Refresh Task Coupling

**Location**: `play_scheduler_refresh.c`

**Current Behavior**:
```c
// Refresh task is tightly coupled to play scheduler
// Hard to abort mid-refresh
// Shares state with pick operations
```

**Problems**:
- **Interrupt difficulty**: New SC can't cleanly abort ongoing refresh
- **Mutex contention**: Refresh holds mutex while waiting for MQTT
- **State complexity**: refresh_pending + refresh_in_progress + refresh_async_pending

**Impact**: Medium — Complexity and occasional hangs

**Solution**: Fully decoupled refresh task with message queue (Phase 5)

---

## 4. Lower Pain Points

### 4.1 Multiple State Machines

**Location**: `app_state.c`, `p3a_state.c`

**Current Behavior**:
- `app_state`: READY/PROCESSING/ERROR (command processing)
- `p3a_state`: Global app states (PLAYBACK/PROVISIONING/OTA/PICO8)
- Both exist, somewhat overlapping

**Problems**:
- **Confusion**: Which state machine to check?
- **Redundancy**: Both track "something is happening"
- **Inconsistency**: Can be in READY + P3A_STATE_OTA simultaneously

**Impact**: Low — Works but confusing

**Solution**: Clearer documentation of roles; potential future consolidation

---

### 4.2 Play Scheduler History vs LAi

**Current Behavior**:
- History stores full `ps_artwork_t` structs
- History entries may reference files no longer in LAi

**Problems**:
- **Stale history**: prev() might try to play evicted file
- **Memory use**: Full artwork struct (256+ bytes) per history entry

**Impact**: Low — Rare edge case

**Solution**: Handle history load failures same as regular load failures

---

### 4.3 Vault Sharding Complexity

**Location**: `vault_storage.c`, `makapix_channel_utils.c`

**Current Behavior**:
```c
// Path: /vault/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
// Requires SHA256 computation for every path lookup
```

**Problems**:
- **CPU overhead**: SHA256 on every path build
- **Deep directories**: Many empty directories
- **LRU complexity**: mtime-based eviction across sharded structure

**Impact**: Low — Sharding prevents directory size issues

**Solution**: Current approach acceptable; could cache SHA prefixes in future

---

## 5. Architectural Decisions Made

### 5.1 Event-Driven Architecture

**Decision**: Migrate from poll-based to event-driven with message queues.

```c
// NEW PATTERN:
void download_task(void *arg) {
    while (true) {
        message_t msg;
        xQueueReceive(queue, &msg, portMAX_DELAY);
        switch (msg.type) {
            case MSG_DOWNLOAD_REQUEST: handle_download(msg.data); break;
            case MSG_FILE_EVICTED: handle_eviction(msg.data); break;
            // ...
        }
    }
}
```

**Benefit**: Eliminates polling, reduces CPU, clearer control flow

---

### 5.2 Single Source of Truth for Availability

**Decision**: LAi is the source of truth; filesystem is just storage.

```c
// NEW PATTERN:
bool is_available(storage_key) {
    return lai_contains(storage_key);
}
```

**Note**: System must still handle load failures gracefully since LAi can become stale.

---

### 5.3 Robustness Over Perfect Tracking

**Decision**: Don't track files across channels; instead handle failures gracefully.

- Same file can be in multiple channels' Ci
- If file is evicted, LAi entries may become stale
- When load fails, remove from LAi and try another
- LTF system prevents infinite re-download loops

---

## 6. Technical Debt Summary

| Category | Severity | Solution Phase |
|----------|----------|----------------|
| File existence checks | Critical | Phase 2-3 |
| Web UI file scanning | Critical | Phase 6 |
| Blocking on refresh | High | Phase 5 |
| Flat connectivity states | Medium | Phase 4 |
| No Ci/LAi atomicity | Medium | Phase 1 |
| Download manager polling | Medium | Phase 5 |
| Multiple state machines | Low | Documentation |

---

*Previous: [05-MIGRATION-PLAN.md](05-MIGRATION-PLAN.md)*  
*Back to: [00-OVERVIEW.md](00-OVERVIEW.md)*
