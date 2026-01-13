# Download Coordination Specification

## Overview

This document specifies how p3a coordinates file downloads to fill "availability holes" in channel indices without blocking animation playback.

## Core Principles

1. **One Download at a Time**: Only one HTTP download active to avoid SD card contention
2. **Round-Robin Across Channels**: Fair distribution of download bandwidth
3. **Non-Blocking**: Downloads never block playback or user interaction
4. **Online Mode Only**: Downloads only happen when `p3a_connectivity_is_online() == true`
5. **Incremental LAi Updates**: Each completed download updates LAi immediately

## Download Manager Architecture

### State Structure

```c
typedef struct {
    char channel_id[64];
    uint32_t dl_cursor;          // Next index to check in Ci
    bool channel_complete;       // All entries scanned this epoch
} dl_channel_state_t;

static struct {
    dl_channel_state_t channels[DL_MAX_CHANNELS];
    size_t channel_count;
    size_t round_robin_idx;      // Current channel for round-robin
    
    bool busy;                   // Download in progress
    char active_channel[64];     // Channel being downloaded
    
    TaskHandle_t task;           // Background download task
    SemaphoreHandle_t mutex;     // Protects state
} s_dl_state;
```

### Download Task

```c
void download_task(void *arg) {
    while (1) {
        // Wait for work signal
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Check online mode
        if (!p3a_connectivity_is_online()) {
            ESP_LOGD(TAG, "Offline - skipping downloads");
            continue;
        }
        
        // Find next file to download
        download_request_t req;
        while (find_next_download(&req) == ESP_OK) {
            if (!p3a_connectivity_is_online()) {
                break;  // Went offline mid-loop
            }
            
            // Perform download
            set_busy(true, req.channel_id);
            esp_err_t err = perform_download(&req);
            set_busy(false, NULL);
            
            if (err == ESP_OK) {
                on_download_complete(&req);
            } else {
                on_download_failed(&req);
            }
        }
    }
}
```

## Finding Next Download

### Algorithm

```c
esp_err_t find_next_download(download_request_t *out_req) {
    // Try each channel in round-robin order
    for (size_t attempt = 0; attempt < s_dl_state.channel_count; attempt++) {
        size_t ch_idx = (s_dl_state.round_robin_idx + attempt) % s_dl_state.channel_count;
        dl_channel_state_t *ch = &s_dl_state.channels[ch_idx];
        
        if (ch->channel_complete) {
            continue;  // Already scanned all entries
        }
        
        // Scan channel from cursor position
        if (scan_channel_for_hole(ch, out_req) == ESP_OK) {
            // Found a file to download
            s_dl_state.round_robin_idx = (ch_idx + 1) % s_dl_state.channel_count;
            return ESP_OK;
        } else {
            // Channel complete
            ch->channel_complete = true;
        }
    }
    
    // All channels complete
    return ESP_ERR_NOT_FOUND;
}
```

### Scanning for Holes

```c
esp_err_t scan_channel_for_hole(dl_channel_state_t *ch, download_request_t *out_req) {
    // Load channel cache
    char cache_path[256];
    build_cache_path(ch->channel_id, cache_path);
    
    FILE *f = fopen(cache_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    
    // Read header
    uint32_t entry_count, lai_count;
    fread(&entry_count, 4, 1, f);
    fread(&lai_count, 4, 1, f);
    
    // Scan from cursor
    for (uint32_t i = ch->dl_cursor; i < entry_count; i++) {
        // Seek to entry
        fseek(f, 8 + i * sizeof(channel_entry_t), SEEK_SET);
        
        channel_entry_t entry;
        fread(&entry, sizeof(entry), 1, f);
        
        // Check if already in LAi
        if (lai_binary_search(f, entry_count, lai_count, i)) {
            continue;  // Already available
        }
        
        // Build filepath
        char filepath[256];
        build_vault_filepath(&entry, filepath);
        
        // Check for 404 marker
        if (has_404_marker(filepath)) {
            continue;  // Permanent failure
        }
        
        // Check if file exists (may be shared from another channel)
        if (file_exists(filepath)) {
            // File exists! Add to LAi
            add_to_lai_on_disk(cache_path, i);
            continue;
        }
        
        // Found a hole!
        fill_download_request(&entry, ch->channel_id, out_req);
        ch->dl_cursor = i + 1;  // Resume from next entry
        fclose(f);
        return ESP_OK;
    }
    
    // Reached end of channel
    ch->dl_cursor = entry_count;
    fclose(f);
    return ESP_ERR_NOT_FOUND;
}
```

**Key Optimizations**:
1. Don't download if file already exists (shared across channels)
2. Skip entries with .404 markers (permanent failures)
3. Resume from cursor (don't re-scan)

## Download Execution

### HTTP Download

```c
esp_err_t perform_download(const download_request_t *req) {
    ESP_LOGI(TAG, "Downloading: %s (channel: %s)", req->storage_key, req->channel_id);
    
    // Create shard directories
    ensure_shard_dirs(req->filepath);
    
    // Download to temp file
    char temp_path[280];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", req->filepath);
    
    esp_err_t err = makapix_artwork_download(req->art_url, temp_path);
    if (err != ESP_OK) {
        // Cleanup temp file
        unlink(temp_path);
        return err;
    }
    
    // Atomic rename
    if (rename(temp_path, req->filepath) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s -> %s", temp_path, req->filepath);
        unlink(temp_path);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Downloaded: %s", req->filepath);
    return ESP_OK;
}
```

### Download Failure Handling

```c
void on_download_failed(const download_request_t *req) {
    // Create .404 marker to prevent retries
    char marker_path[280];
    snprintf(marker_path, sizeof(marker_path), "%s.404", req->filepath);
    
    FILE *f = fopen(marker_path, "w");
    if (f) {
        fprintf(f, "Download failed at %lu\n", (unsigned long)time(NULL));
        fclose(f);
    }
    
    ESP_LOGW(TAG, "Download failed, marked as 404: %s", req->storage_key);
}
```

**Rationale**: Prevents infinite retry loops for permanently unavailable files.

## Download Completion

### Adding to LAi

```c
void on_download_complete(const download_request_t *req) {
    // 1. Load cache file
    char cache_path[256];
    build_cache_path(req->channel_id, cache_path);
    
    // 2. Add entry to LAi
    uint32_t ci_index = find_ci_index(cache_path, req->storage_key);
    if (ci_index != UINT32_MAX) {
        add_to_lai_on_disk(cache_path, ci_index);
    }
    
    // 3. Check if this was the first entry for this channel
    uint32_t lai_count = get_lai_count(cache_path);
    if (lai_count == 1) {
        // First entry - may need to trigger playback
        trigger_playback_if_needed();
    }
    
    // 4. Signal download manager to continue
    download_manager_signal_work_available();
}
```

### Triggering Playback

```c
void trigger_playback_if_needed(void) {
    // Check if playback is already active
    extern bool animation_player_is_animation_ready(void) __attribute__((weak));
    if (animation_player_is_animation_ready && animation_player_is_animation_ready()) {
        return;  // Already playing
    }
    
    // Check if any LAi has entries now
    bool has_entries = false;
    for (size_t i = 0; i < s_channel_count; i++) {
        uint32_t lai_count = get_lai_count_for_channel(s_channels[i].channel_id);
        if (lai_count > 0) {
            has_entries = true;
            break;
        }
    }
    
    if (has_entries) {
        ESP_LOGI(TAG, "First entry available - starting playback");
        play_scheduler_next(NULL);
    }
}
```

**Edge Case**: This handles the "cold start" scenario where:
1. SC arrives with empty LAi
2. Refresh completes, but no files downloaded yet
3. First download completes → `next()` triggers playback

## Coordination with Play Scheduler

### Setting Active Channels

```c
// Called from play_scheduler_execute_command()
void download_manager_set_channels(const char **channel_ids, size_t count) {
    xSemaphoreTake(s_dl_state.mutex, portMAX_DELAY);
    
    s_dl_state.channel_count = count;
    for (size_t i = 0; i < count; i++) {
        strlcpy(s_dl_state.channels[i].channel_id, channel_ids[i], 64);
        s_dl_state.channels[i].dl_cursor = 0;
        s_dl_state.channels[i].channel_complete = false;
    }
    s_dl_state.round_robin_idx = 0;
    
    xSemaphoreGive(s_dl_state.mutex);
    
    // Wake download task
    download_manager_signal_work_available();
}
```

### Resetting on Refresh

```c
// Called when channel refresh completes
void download_manager_reset_cursors(void) {
    xSemaphoreTake(s_dl_state.mutex, portMAX_DELAY);
    
    for (size_t i = 0; i < s_dl_state.channel_count; i++) {
        s_dl_state.channels[i].dl_cursor = 0;
        s_dl_state.channels[i].channel_complete = false;
    }
    
    xSemaphoreGive(s_dl_state.mutex);
    
    // Wake download task
    download_manager_signal_work_available();
}
```

**Rationale**: New entries may have been added during refresh, rescan from beginning.

## File Sharing Across Channels

### Deduplication

```c
// Before downloading, check if file already exists
if (file_exists(filepath)) {
    ESP_LOGI(TAG, "File already exists (shared from another channel): %s", storage_key);
    
    // Add to this channel's LAi without re-downloading
    add_to_lai_on_disk(cache_path, ci_index);
    
    return ESP_OK;
}
```

**Example**: 
- Artwork X is in both "promoted" and "all" channels
- "promoted" refreshes first, downloads file
- "all" refreshes later, sees file already exists, adds to LAi without re-downloading

### Vault Sharding

Files are stored in sharded directories to avoid filesystem limits:

```
/sdcard/p3a/vault/
  ├─ 12/
  │   ├─ 34/
  │   │   ├─ 56/
  │   │   │   ├─ uuid-1234.webp
  │   │   │   └─ uuid-5678.gif
```

Sharding based on SHA256(storage_key):

```c
void build_vault_filepath(const channel_entry_t *entry, char *out, size_t len) {
    uint8_t sha256[32];
    storage_key_sha256(entry->storage_key, sha256);
    
    snprintf(out, len, "%s/%02x/%02x/%02x/%s.%s",
             vault_base,
             sha256[0], sha256[1], sha256[2],
             entry->storage_key,
             extension_strings[entry->extension]);
}
```

## Download Bandwidth Management

### Rate Limiting

```c
// In download_task loop
while (find_next_download(&req) == ESP_OK) {
    perform_download(&req);
    
    // Brief pause between downloads to avoid saturating Wi-Fi
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

**Configurable**: Could add config option for download throttling.

### Priority Scheduling

Currently: Simple round-robin across channels.

**Future Enhancement**: Prioritize channels with smaller LAi to ensure variety.

```c
// Sort channels by lai_count ascending
// Download from channels with fewer entries first
```

## Error Recovery

### Transient Failures

```c
if (http_status == 503 || http_status == 429) {
    // Server overload or rate limit
    // Don't create .404 marker
    ESP_LOGW(TAG, "Transient error %d, will retry later", http_status);
    return ESP_ERR_HTTP_BASE + http_status;
}
```

**Retry Strategy**: Next scan will retry (no .404 marker).

### Permanent Failures

```c
if (http_status == 404 || http_status == 410) {
    // File doesn't exist or was deleted
    create_404_marker(filepath);
    return ESP_ERR_NOT_FOUND;
}
```

**No Retry**: .404 marker prevents infinite attempts.

### Network Failures

```c
if (err == ESP_ERR_HTTP_CONNECT) {
    // Network issue
    ESP_LOGW(TAG, "Network error during download");
    
    // Check connectivity state
    if (!p3a_connectivity_is_online()) {
        ESP_LOGI(TAG, "Went offline - stopping downloads");
        break;  // Exit download loop
    }
    
    // Transient network issue - will retry
    return err;
}
```

## SD Card Contention Management

### SD Access Conflicts

**Problem**: Simultaneous SD access from multiple tasks:
- Download task writing files
- Play scheduler reading cache files
- Animation player reading image files

**Solution**:
1. Download writes to `.tmp` file, atomic rename when complete
2. Cache file reads are read-only (no locking needed)
3. Animation player reads are sequential (no seeking)

### Worst-Case Scenario

```
T+0ms: Download writing /vault/12/34/56/art1.webp.tmp
T+10ms: Play scheduler reads /channel/promoted.bin
T+20ms: Animation player reads /vault/78/90/ab/art2.webp
T+30ms: Download renames art1.webp.tmp -> art1.webp
```

**No Conflicts**: Different files, no locking needed.

## Web UI Impact

### Download Progress Display

```c
// REST API endpoint: GET /api/download/status
{
    "downloading": true,
    "channel_id": "promoted",
    "storage_key": "abc-123-def",
    "filename": "uuid-1234.webp",
    "progress_percent": 45,  // -1 if unknown
    "queue_size": 127        // Approximate files remaining
}
```

### Triggering Downloads

```c
// Web UI can signal download manager after operations
// POST /api/channel/refresh
{
    "channel": "promoted"
}

// Response triggers:
// 1. makapix_refresh_channel_index()
// 2. download_manager_reset_cursors()
// 3. download_manager_signal_work_available()
```

## Performance Metrics

### Target Rates

- **Download Speed**: Limited by Wi-Fi (typically 1-5 MB/s)
- **File Size**: Typical animated WebP is 100-500 KB
- **Download Time**: ~1-5 seconds per file
- **Channel Fill Time**: For 100 missing files = 2-8 minutes

### Optimization Opportunities

1. **Parallel Downloads**: 2-3 concurrent downloads (more complex SD coordination)
2. **Prefetch on Peek**: Start downloading `peek_next()` result
3. **Smart Priority**: Download artworks likely to be seen next

**Trade-offs**: Complexity vs bandwidth vs responsiveness.

## Open Questions

### Q1: Maximum Download Retries
**Question**: How many times to retry transient download failures?
**Options**:
- A) Retry once per scan pass (simple)
- B) Track retry count per entry (3 max)
- C) Exponential backoff with timestamps

**Recommendation**: Option A for simplicity. If it fails across multiple scans, likely a persistent issue.

### Q2: Download During Playback
**Question**: Should downloads pause during playback to reduce SD contention?
**Options**:
- A) Continuous downloads (current)
- B) Pause downloads during frame decode
- C) Rate limit downloads when playback active

**Recommendation**: Option A unless frame drops are observed in testing. Modern SD cards can handle concurrent access.

### Q3: Bandwidth Limiting
**Question**: Should we limit download bandwidth to avoid saturating Wi-Fi?
**Options**:
- A) No limit (greedy)
- B) Hard limit (e.g., 1 MB/s max)
- C) Adaptive (slow down if MQTT latency increases)

**Recommendation**: Option A initially. Add config option if users report Wi-Fi issues.

### Q4: Queue Size Estimation
**Question**: How to estimate remaining download queue size for UI display?
**Options**:
- A) Count holes in all channel caches (expensive)
- B) Track downloads completed since last reset
- C) Don't estimate (just show "downloading")

**Recommendation**: Option B. Simple counter: `remaining = total_entries - lai_total - downloads_completed`.

## Migration Pain Points

1. **LAi Updates**: Need atomic add-to-LAi function that works on disk
2. **Download Cursor**: Existing code doesn't track cursors, need to initialize
3. **Shared Files**: Need to detect and avoid re-downloading shared files
4. **Error Handling**: .404 marker creation needs to be robust
5. **Testing**: Need to simulate various network failure modes

## Testing Strategy

### Unit Tests
- Hole detection with various LAi states
- .404 marker creation and checking
- Round-robin channel selection
- Shared file detection

### Integration Tests
- Download → add to LAi → trigger playback
- Refresh → reset cursors → rescan
- Network failure → recovery
- Shared file → no re-download

### Stress Tests
- 1000 missing files across 10 channels
- Rapid refresh commands
- Network disconnect mid-download
- SD card space exhaustion
