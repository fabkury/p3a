# p3a State Machine — Locally Available Index (LAi)

**Document:** 02-LOCALLY-AVAILABLE-INDEX.md  
**Status:** Final Specification

---

## 1. Problem Statement

Currently, the Play Scheduler performs **on-the-fly file existence checks** when picking artworks:

```c
// In play_scheduler_pick.c
static bool is_entry_available(const makapix_channel_entry_t *entry) {
    char filepath[256];
    build_vault_filepath(entry, filepath, sizeof(filepath));
    return file_exists(filepath) && !has_404_marker(filepath);
}
```

This approach has significant problems:

1. **Slow random picks**: Random mode may need to check 5+ entries before finding an available one
2. **I/O during playback**: File system access on every pick attempt
3. **No count available**: Can't efficiently know how many artworks are available
4. **Repeated scanning**: Same files checked repeatedly across multiple picks

---

## 2. The LAi Concept

### 2.1 Definition

**LAi (Locally Available index)** is a per-channel array containing only the artworks that are:
1. Present in the channel index (Ci)
2. Downloaded locally to the vault
3. Not marked with a 404 error or terminal load failure

### 2.2 Key Properties

- **LAi ⊆ Ci**: Every entry in LAi exists in Ci
- **|LAi| ≤ |Ci|**: LAi size is at most equal to Ci size
- **LAi is persisted**: Stored alongside Ci for atomic recovery
- **LAi is maintained incrementally**: Updated on download complete / file eviction / load failure
- **LAi is not the absolute truth**: System must handle failures gracefully when loading files from LAi

---

## 3. Data Structure

### 3.1 Unified Cache File Format

Store both Ci and LAi in a single file for atomic persistence:

```c
/**
 * @brief Channel cache file header
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              // 0x50334143 ('P3AC')
    uint16_t version;            // Format version (1)
    uint16_t flags;              // Reserved
    uint32_t ci_count;           // Number of entries in Ci
    uint32_t lai_count;          // Number of entries in LAi
    uint32_t ci_offset;          // Byte offset to Ci array
    uint32_t lai_offset;         // Byte offset to LAi array
    uint32_t checksum;           // CRC32 of entire file
    uint8_t reserved[16];        // Future use
} channel_cache_header_t;

// File layout:
// [header: 40 bytes]
// [Ci entries: ci_count * 64 bytes]
// [LAi indices: lai_count * 4 bytes]  // Indices into Ci
```

### 3.2 LAi Storage Format

LAi is stored as an array of `uint32_t` indices into Ci:
- Compact: 4 bytes per available artwork
- Fast lookup: O(1) random access
- Easy maintenance: add/remove indices

### 3.3 In-Memory Representation

```c
typedef struct {
    // Channel index (Ci) - all known artworks
    makapix_channel_entry_t *entries;
    size_t entry_count;
    
    // Locally available index (LAi) - indices of available artworks
    uint32_t *available_indices;
    size_t available_count;
    
    // Metadata
    char channel_id[64];
    uint32_t cache_version;
    bool dirty;                   // Needs persistence
} channel_cache_t;
```

---

## 4. Operations

### 4.1 Random Pick from LAi

```c
static bool pick_random_from_lai(channel_cache_t *cache, uint32_t *out_ci_index) {
    if (cache->available_count == 0) {
        return false;  // Empty LAi
    }
    
    // Random index in LAi
    uint32_t lai_idx = prng_next() % cache->available_count;
    
    // Get Ci index
    *out_ci_index = cache->available_indices[lai_idx];
    return true;
}
```

**Note**: LAi does not guarantee file actually exists — caller must handle load failures gracefully.

### 4.2 Sequential Pick from LAi

```c
static bool pick_sequential_from_lai(channel_cache_t *cache, 
                                     uint32_t *cursor,
                                     uint32_t *out_ci_index) {
    if (cache->available_count == 0) {
        return false;
    }
    
    // Get entry at cursor (modulo available count)
    uint32_t lai_idx = (*cursor) % cache->available_count;
    *out_ci_index = cache->available_indices[lai_idx];
    
    // Advance cursor
    (*cursor)++;
    return true;
}
```

### 4.3 Add Entry to LAi (Download Complete)

```c
void lai_add_entry(channel_cache_t *cache, uint32_t ci_index) {
    // Check if already in LAi
    for (size_t i = 0; i < cache->available_count; i++) {
        if (cache->available_indices[i] == ci_index) {
            return;  // Already present
        }
    }
    
    // Add to LAi
    cache->available_indices[cache->available_count++] = ci_index;
    cache->dirty = true;
    
    // Schedule async persistence (15-second debounce)
    schedule_cache_save(cache);
}
```

### 4.4 Remove Entry from LAi (File Evicted or Load Failed)

```c
void lai_remove_entry(channel_cache_t *cache, uint32_t ci_index) {
    for (size_t i = 0; i < cache->available_count; i++) {
        if (cache->available_indices[i] == ci_index) {
            // Swap with last and shrink
            cache->available_indices[i] = cache->available_indices[--cache->available_count];
            cache->dirty = true;
            schedule_cache_save(cache);
            return;
        }
    }
}
```

### 4.5 Rebuild LAi from Ci

On cache load or when consistency is suspect:

```c
void lai_rebuild(channel_cache_t *cache) {
    cache->available_count = 0;
    
    for (size_t i = 0; i < cache->entry_count; i++) {
        char filepath[256];
        build_vault_filepath(&cache->entries[i], filepath, sizeof(filepath));
        
        if (file_exists(filepath) && 
            !has_404_marker(filepath) && 
            !has_terminal_ltf(filepath)) {
            cache->available_indices[cache->available_count++] = i;
        }
    }
    
    cache->dirty = true;
    schedule_cache_save(cache);
    
    ESP_LOGI(TAG, "Rebuilt LAi: %zu/%zu available", 
             cache->available_count, cache->entry_count);
}
```

**Note**: Rebuild is expensive (O(n) file checks) and should only happen:
- On first boot with existing cache (migration from old format)
- On suspected inconsistency
- Never during normal operation

**Maximum rebuild time**: ~5 seconds for 1,024 entries — acceptable for migration.

---

## 5. Load Tracker Files (LTF) — Preventing Infinite Re-Download Loops

### 5.1 Problem Statement

When a file in LAi fails to load (corrupted file, truncated download, etc.), the system should:
1. Evict the file from LAi
2. Delete the corrupted file
3. Allow re-download

However, if a file is **inherently corrupted** (bad source), this creates an infinite loop:
- Download → Load fails → Delete → Re-download → Load fails → ...

### 5.2 Solution: Load Tracker Files

Use a **Load Tracker File (LTF)** to track failed load attempts and prevent infinite re-downloads:

```
/sdcard/p3a/vault/{hash}/{storage_key}.ltf
```

The LTF is a small JSON file:

```json
{
    "attempts": 2,
    "last_failure": "2026-01-13T10:30:00Z",
    "reason": "decode_error"
}
```

### 5.3 LTF State Machine

```
File Load Attempted
       │
       ▼
   ┌───────┐
   │ Load  │
   │Success│────► Delete LTF (if exists), file stays in LAi
   └───────┘
       │
       ▼ (Load Failed)
   ┌────────────────┐
   │ Check LTF      │
   │ attempts count │
   └────────────────┘
       │
       ├── attempts < 2 ──► Create/Update LTF (attempts++)
       │                    Delete file
       │                    Remove from LAi
       │                    Allow re-download
       │
       └── attempts >= 2 ──► Create Terminal LTF (attempts = 3, terminal = true)
                             Delete file
                             Remove from LAi
                             Block future downloads
```

### 5.4 LTF Implementation

```c
typedef struct {
    uint8_t attempts;        // 0, 1, 2, or 3 (terminal)
    bool terminal;           // If true, no more re-downloads allowed
    time_t last_failure;
    char reason[32];
} load_tracker_t;

// Check if file can be downloaded
bool ltf_can_download(const char *storage_key) {
    load_tracker_t ltf;
    if (!ltf_load(storage_key, &ltf)) {
        return true;  // No LTF = can download
    }
    return !ltf.terminal;  // Can download if not terminal
}

// Called when file load fails
void ltf_record_failure(const char *storage_key, const char *reason) {
    load_tracker_t ltf = {0};
    ltf_load(storage_key, &ltf);  // May fail if doesn't exist
    
    ltf.attempts++;
    ltf.last_failure = time(NULL);
    strlcpy(ltf.reason, reason, sizeof(ltf.reason));
    
    if (ltf.attempts >= 3) {
        ltf.terminal = true;
        ESP_LOGW(TAG, "File %s marked terminal after %d failures", 
                 storage_key, ltf.attempts);
    }
    
    ltf_save(storage_key, &ltf);
}

// Called when file loads successfully
void ltf_clear(const char *storage_key) {
    ltf_delete(storage_key);
}
```

### 5.5 Integration with Download Manager

Before downloading a file, check the LTF:

```c
// In download_manager.c
bool should_download_file(const char *storage_key) {
    // Check if file exists
    if (file_exists(build_path(storage_key))) {
        return false;  // Already have it
    }
    
    // Check if blocked by terminal LTF
    if (!ltf_can_download(storage_key)) {
        ESP_LOGD(TAG, "Skipping %s: terminal LTF exists", storage_key);
        return false;
    }
    
    // Check .404 marker
    if (has_404_marker(storage_key)) {
        return false;
    }
    
    return true;
}
```

### 5.6 Integration with Play Scheduler (Load Failure Handling)

When the animation player fails to load a file picked from LAi:

```c
// In play_scheduler.c
void on_artwork_load_failed(ps_artwork_t *artwork, const char *reason) {
    ESP_LOGW(TAG, "Failed to load artwork %s: %s", artwork->storage_key, reason);
    
    // 1. Record the failure in LTF
    ltf_record_failure(artwork->storage_key, reason);
    
    // 2. Delete the corrupted file
    char filepath[256];
    build_vault_filepath(artwork->storage_key, filepath, sizeof(filepath));
    unlink(filepath);
    
    // 3. Remove from LAi
    channel_cache_t *cache = get_channel_cache(artwork->channel_id);
    uint32_t ci_idx = find_ci_index(cache, artwork->storage_key);
    if (ci_idx != UINT32_MAX) {
        lai_remove_entry(cache, ci_idx);
    }
    
    // 4. Pick another artwork and continue
    play_scheduler_next(NULL);
}
```

---

## 6. Persistence

### 6.1 File Path

```
/sdcard/p3a/channel/{channel_id}.bin
```

Same as current, but with new header format.

### 6.2 Persistence Frequency

Use a **15-second debounce** for LAi persistence:
- On LAi change, mark cache dirty and start/restart timer
- When timer expires, persist to disk
- Prevents excessive SD card writes during download bursts

### 6.3 Atomic Save

```c
esp_err_t cache_save(const channel_cache_t *cache) {
    char path[256], tmp_path[260];
    snprintf(path, sizeof(path), "/sdcard/p3a/channel/%s.bin", cache->channel_id);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return ESP_FAIL;
    
    // Write header
    channel_cache_header_t header = {
        .magic = 0x50334143,
        .version = 1,
        .ci_count = cache->entry_count,
        .lai_count = cache->available_count,
        .ci_offset = sizeof(header),
        .lai_offset = sizeof(header) + cache->entry_count * 64,
    };
    fwrite(&header, sizeof(header), 1, f);
    
    // Write Ci
    fwrite(cache->entries, 64, cache->entry_count, f);
    
    // Write LAi
    fwrite(cache->available_indices, 4, cache->available_count, f);
    
    // Compute and write checksum
    // ... compute CRC32 and seek back to write it ...
    
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    // Atomic rename
    rename(tmp_path, path);
    
    return ESP_OK;
}
```

### 6.4 Load and Validate

```c
esp_err_t cache_load(const char *channel_id, channel_cache_t *cache) {
    char path[256];
    snprintf(path, sizeof(path), "/sdcard/p3a/channel/%s.bin", channel_id);
    
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    
    // Read and validate header
    channel_cache_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1 ||
        header.magic != 0x50334143 ||
        header.version != 1) {
        fclose(f);
        return ESP_ERR_INVALID_VERSION;  // Triggers migration/rebuild
    }
    
    // Allocate and read Ci
    cache->entries = malloc(header.ci_count * 64);
    fseek(f, header.ci_offset, SEEK_SET);
    fread(cache->entries, 64, header.ci_count, f);
    cache->entry_count = header.ci_count;
    
    // Allocate and read LAi
    cache->available_indices = malloc(header.lai_count * 4);
    fseek(f, header.lai_offset, SEEK_SET);
    fread(cache->available_indices, 4, header.lai_count, f);
    cache->available_count = header.lai_count;
    
    // Verify checksum
    // ... if checksum fails, trigger lai_rebuild() ...
    
    fclose(f);
    return ESP_OK;
}
```

---

## 7. Migration Strategy

### 7.1 Old Format Detection

If file doesn't have the new magic header, treat as old format:

```c
if (header.magic != 0x50334143) {
    // Old format: raw array of makapix_channel_entry_t
    // Load as Ci, then rebuild LAi
    cache_load_legacy(f, cache);
    lai_rebuild(cache);
    cache_save(cache);  // Upgrade to new format
}
```

### 7.2 First Boot with Existing Data

On first boot after firmware update:
1. Detect old format
2. Load Ci entries
3. Rebuild LAi (scan files)
4. Save in new format

This is a one-time cost per channel (~5 seconds for 1,024 entries).

---

## 8. Memory Considerations

For a channel with 1,024 entries:
- **Ci**: 1,024 × 64 = 65,536 bytes
- **LAi**: 1,024 × 4 = 4,096 bytes (worst case, all available)
- **Total**: ~70 KB per channel

With multiple channels loaded:
- 3 channels: ~210 KB
- Fits comfortably in ESP32-P4's 32MB PSRAM

---

## 9. Integration with Download Manager

### 9.1 On Download Complete

```c
// In download_manager.c
if (download_succeeded) {
    // Clear any existing LTF (successful download proves file is valid)
    ltf_clear(storage_key);
    
    // Find Ci index for this entry
    uint32_t ci_idx = find_ci_index(cache, storage_key);
    if (ci_idx != UINT32_MAX) {
        lai_add_entry(cache, ci_idx);
    }
}
```

### 9.2 File Selection for Download

When picking the next file to download, the download manager should:
1. Round-robin through current channels (not weighted by LAi size)
2. For each channel, scan Ci for entries not in LAi
3. Skip entries with `.404` markers
4. Skip entries with terminal LTF

```c
bool should_download_entry(channel_cache_t *cache, uint32_t ci_idx) {
    const char *storage_key = cache->entries[ci_idx].storage_key;
    
    // Skip if already in LAi
    if (lai_contains(cache, ci_idx)) {
        return false;
    }
    
    // Skip if blocked by terminal LTF
    if (!ltf_can_download(storage_key)) {
        return false;
    }
    
    // Skip if has 404 marker
    if (has_404_marker(storage_key)) {
        return false;
    }
    
    return true;
}
```

---

## 10. Edge Case: All Files Deleted from SD Card

### 10.1 Scenario

Ci and LAi loaded with no issues, but all artwork files have been deleted from the SD card (user formatted card, external deletion, etc.). All LAi entries now point to non-existent files.

### 10.2 Required Behavior

- **No infinite loops**: System must not get stuck trying to play missing files
- **No prohibitive lags**: Several seconds acceptable, but not minutes
- **No crashes**: Must handle gracefully
- **Graceful fallback**: Display "no artworks available" if offline, or attempt downloads if online

### 10.3 Implementation Strategy

When an artwork picked from LAi fails to load:

```c
// Play Scheduler load failure handler
void on_artwork_load_failed(ps_artwork_t *artwork, const char *reason) {
    // Record failure, delete file (if exists), remove from LAi
    handle_load_failure(artwork, reason);
    
    // Try picking another artwork
    // This will naturally exhaust LAi if all files are missing
    if (get_total_lai_count() > 0) {
        // Still have entries in LAi - try another
        play_scheduler_next(NULL);
    } else {
        // LAi is now empty - show appropriate message
        if (connectivity_state_get() == CONN_STATE_ONLINE) {
            show_message("Downloading artworks...");
            download_manager_signal_work_available();
        } else {
            show_message("No artworks available");
        }
    }
}
```

Key points:
- Each failed load removes the entry from LAi
- Eventually LAi becomes empty
- System transitions to "waiting for downloads" or "no artworks" state
- No infinite loop because each iteration removes an entry

### 10.4 Performance Consideration

With 1,024 entries all missing:
- Each load attempt: ~50ms (file open fails quickly)
- Total time to exhaust LAi: ~50 seconds worst case
- **Optimization**: Batch check first 10 entries on startup; if all fail, trigger full LAi rebuild

```c
// Optional startup optimization
bool lai_quick_validate(channel_cache_t *cache, size_t sample_size) {
    size_t failures = 0;
    size_t samples = min(sample_size, cache->available_count);
    
    for (size_t i = 0; i < samples; i++) {
        uint32_t ci_idx = cache->available_indices[i];
        char filepath[256];
        build_vault_filepath(&cache->entries[ci_idx], filepath, sizeof(filepath));
        if (!file_exists(filepath)) {
            failures++;
        }
    }
    
    // If >80% of sample is missing, likely all files deleted
    return failures < (samples * 0.8);
}
```

---

*Previous: [01-CONNECTIVITY-STATE.md](01-CONNECTIVITY-STATE.md)*  
*Next: [03-PLAY-SCHEDULER-BEHAVIOR.md](03-PLAY-SCHEDULER-BEHAVIOR.md)*
