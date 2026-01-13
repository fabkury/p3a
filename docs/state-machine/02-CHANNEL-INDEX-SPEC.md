# Channel Index Specification: Ci and LAi

## Overview

This document specifies the architecture for channel indices, focusing on the critical distinction between:
- **Ci** (Channel Index): All artworks in the channel (up to 1,024 entries)
- **LAi** (Locally Available Index): Subset of Ci that are actually downloaded and playable

## The Problem

**Current State**: Channel cache files contain all entries from the channel, regardless of whether files are downloaded. When Play Scheduler picks a random artwork:

```c
// Current problematic approach
int idx = random() % channel_entry_count;
entry = channel_entries[idx];
if (!file_exists(entry.filepath)) {
    // Retry - but how many times? What if 90% of files are missing?
    goto retry;
}
```

This leads to:
- Retry loops when most files are missing
- Inefficient random selection
- Complex "skip unavailable" logic scattered throughout codebase

**Desired State**: Play Scheduler only sees available files:

```c
// Desired approach with LAi
int idx = random() % LAi_count;  // LAi_count may be much smaller than Ci_count
entry = LAi[idx];  // Guaranteed to exist locally
play(entry);  // No retries needed
```

## LAi Architecture

### Data Structures

```c
typedef struct {
    uint32_t post_id;           // Unique artwork ID
    uint8_t storage_key_uuid[16];  // UUID bytes
    uint8_t extension;          // 0=webp, 1=gif, 2=png, 3=jpg
    uint32_t created_at;        // Unix timestamp
    uint32_t dwell_time_ms;     // Per-artwork dwell override
    // ... other metadata
} channel_entry_t;

typedef struct {
    uint32_t entry_count;       // Number of entries in Ci
    uint32_t lai_count;         // Number of entries in LAi (lai_count <= entry_count)
    channel_entry_t entries[];  // All channel entries (Ci)
    uint32_t lai_indices[];     // Indices into entries[] for available artworks
} channel_cache_file_t;
```

**File Layout on SD Card**:
```
/sdcard/p3a/channel/promoted.bin:
  [Header: 8 bytes]
    uint32_t entry_count
    uint32_t lai_count
  [Ci Section: entry_count * sizeof(channel_entry_t)]
    entry[0]
    entry[1]
    ...
    entry[entry_count-1]
  [LAi Section: lai_count * 4 bytes]
    lai_indices[0]  // Index into Ci
    lai_indices[1]
    ...
    lai_indices[lai_count-1]
```

### Benefits of This Design

1. **Single Atomic File**: Ci and LAi are always synchronized (no race conditions)
2. **Efficient Random Access**: Pick from LAi in O(1) time
3. **Memory Efficient**: LAi is just an array of indices (4 bytes per entry)
4. **Fast Availability Check**: Binary search in LAi to check if entry is available

### Example

```
Channel "promoted" has 100 entries, but only 10 are downloaded:

Ci = [E0, E1, E2, ..., E99]  // 100 entries
LAi = [0, 5, 12, 23, 45, 51, 67, 78, 89, 92]  // 10 indices

To pick random available artwork:
  idx = random() % 10;  // 0-9
  lai_idx = LAi[idx];   // e.g., 23
  entry = Ci[lai_idx];  // E23
  filepath = build_filepath(entry);
  play(filepath);  // Guaranteed to exist
```

## LAi Operations

### Building LAi (Initial Scan)

When loading a channel cache from disk:

```c
esp_err_t rebuild_lai_from_ci(channel_state_t *ch) {
    ch->lai_count = 0;
    
    for (uint32_t i = 0; i < ch->entry_count; i++) {
        channel_entry_t *entry = &ch->entries[i];
        char filepath[256];
        build_vault_filepath(entry, filepath, sizeof(filepath));
        
        if (file_exists(filepath) && !has_404_marker(filepath)) {
            ch->lai_indices[ch->lai_count++] = i;
        }
    }
    
    // Persist updated LAi to disk
    return save_channel_cache(ch);
}
```

**When to rebuild LAi:**
- On boot (first load of each channel)
- After bulk operations (e.g., SD card swap)
- **NOT** on every channel refresh (too expensive)

### Adding to LAi (File Downloaded)

```c
void lai_add_entry(channel_state_t *ch, uint32_t ci_index) {
    // Check if already in LAi
    for (uint32_t i = 0; i < ch->lai_count; i++) {
        if (ch->lai_indices[i] == ci_index) {
            return;  // Already present
        }
    }
    
    // Add to LAi
    ch->lai_indices[ch->lai_count++] = ci_index;
    
    // Persist to disk
    save_channel_cache(ch);
}
```

**When to call:**
- After successful file download
- When scanning finds a file that should be in LAi but isn't

### Removing from LAi (File Deleted or Failed)

```c
void lai_remove_entry(channel_state_t *ch, uint32_t ci_index) {
    // Find entry in LAi
    for (uint32_t i = 0; i < ch->lai_count; i++) {
        if (ch->lai_indices[i] == ci_index) {
            // Remove by shifting remaining entries
            memmove(&ch->lai_indices[i], 
                    &ch->lai_indices[i + 1],
                    (ch->lai_count - i - 1) * sizeof(uint32_t));
            ch->lai_count--;
            
            // Persist to disk
            save_channel_cache(ch);
            return;
        }
    }
}
```

**When to call:**
- After LRU eviction deletes a file
- When file fails to load/decode (mark as permanently unavailable with .404)
- When user manually deletes files via USB mass storage

### Checking Availability

```c
bool lai_contains(channel_state_t *ch, uint32_t ci_index) {
    // Binary search if LAi is sorted, or linear search
    for (uint32_t i = 0; i < ch->lai_count; i++) {
        if (ch->lai_indices[i] == ci_index) {
            return true;
        }
    }
    return false;
}
```

## Channel Refresh Integration

When a channel index refresh completes (new entries from MQTT):

```c
esp_err_t channel_refresh_complete(channel_state_t *ch, 
                                   channel_entry_t *new_entries, 
                                   uint32_t new_count) {
    // 1. Update Ci with new entries
    free(ch->entries);
    ch->entries = new_entries;
    ch->entry_count = new_count;
    
    // 2. Rebuild LAi by scanning which new entries are already downloaded
    //    (Many may already exist if files are shared across channels)
    ch->lai_count = 0;
    for (uint32_t i = 0; i < new_count; i++) {
        char filepath[256];
        build_vault_filepath(&new_entries[i], filepath, sizeof(filepath));
        
        if (file_exists(filepath) && !has_404_marker(filepath)) {
            ch->lai_indices[ch->lai_count++] = i;
        }
    }
    
    // 3. Atomic write: Ci + LAi to disk
    return save_channel_cache(ch);
}
```

**Key Insight**: After refresh, many files may already be downloaded (shared across channels), so LAi is not empty.

## File Format Details

### SD Card Channels (sdcard_index_entry_t)

```c
typedef struct {
    int32_t post_id;           // Negative values for SD card
    uint8_t extension;         // File extension enum
    char filename[150];        // Relative path within SD card
} sdcard_index_entry_t;  // 160 bytes

// SD card LAi uses same lai_indices[] approach
```

### Makapix Channels (makapix_channel_entry_t)

```c
typedef struct {
    int32_t post_id;           // Positive values for Makapix
    uint8_t storage_key_uuid[16];
    uint8_t extension;
    uint32_t created_at;
    uint32_t dwell_time_ms;
    // ... other fields
} makapix_channel_entry_t;  // 64 bytes

// Makapix LAi uses same lai_indices[] approach
```

## Play Scheduler Integration

### Current Code (Without LAi)

```c
// play_scheduler_pick.c
bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out) {
    // Iterate channels with SWRR
    size_t ch_idx = ps_swrr_select_channel(state);
    ps_channel_state_t *ch = &state->channels[ch_idx];
    
    // Pick from channel
    uint32_t cursor = ch->cursor;
    for (int retries = 0; retries < ch->entry_count; retries++) {
        uint32_t idx = (cursor + retries) % ch->entry_count;
        channel_entry_t *entry = &ch->entries[idx];
        
        char filepath[256];
        build_filepath(entry, filepath);
        if (file_exists(filepath)) {
            // Found available entry
            ch->cursor = (idx + 1) % ch->entry_count;
            return build_artwork_from_entry(entry, filepath, out);
        }
    }
    return false;  // No available entries
}
```

### Proposed Code (With LAi)

```c
// play_scheduler_pick.c
bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out) {
    // Iterate channels with SWRR
    size_t ch_idx = ps_swrr_select_channel(state);
    ps_channel_state_t *ch = &state->channels[ch_idx];
    
    if (ch->lai_count == 0) {
        // No available entries - channel is empty for our purposes
        return false;
    }
    
    // Pick from LAi (no retries needed!)
    uint32_t lai_cursor = ch->lai_cursor % ch->lai_count;
    uint32_t ci_idx = ch->lai_indices[lai_cursor];
    channel_entry_t *entry = &ch->entries[ci_idx];
    
    ch->lai_cursor++;
    
    char filepath[256];
    build_filepath(entry, filepath);
    return build_artwork_from_entry(entry, filepath, out);
}
```

**Benefits:**
- No retry loop
- O(1) pick time
- Clear semantics: empty LAi = nothing to play
- No "skip unavailable" logic

## Memory Usage Analysis

### Without LAi (Current)
- Channel with 1,024 entries × 64 bytes = 64 KB per channel
- No additional LAi storage

### With LAi (Proposed)
- Channel with 1,024 entries × 64 bytes = 64 KB (Ci)
- LAi: 1,024 indices × 4 bytes = 4 KB
- **Total: 68 KB per channel** (+6% overhead)

For 16 channels: 64 KB × 16 = 1,088 KB (current) vs 1,152 KB (proposed) = **+64 KB total**

**Verdict**: Negligible memory cost for massive simplification.

## Migration Strategy

### Phase 1: Add LAi Support (Backward Compatible)
1. Modify cache file format to include LAi section
2. When loading old cache files (no LAi section), rebuild LAi from scratch
3. All new writes include LAi section

### Phase 2: Update Play Scheduler
1. Change `ps_pick_next_available()` to use LAi
2. Remove retry/skip logic
3. Treat `lai_count == 0` as "channel empty"

### Phase 3: Update Download Manager
1. Call `lai_add_entry()` after successful download
2. Remove any "mark available" hacks

### Phase 4: Update LRU Eviction
1. Call `lai_remove_entry()` before deleting files
2. Ensure .404 markers also remove from LAi

## Open Questions

### Q1: LAi Sorting
**Question**: Should LAi be sorted by Ci index, or by most-recently-added?
**Options**:
- A) Sorted by Ci index (enables binary search for `lai_contains`)
- B) Insertion order (easier to maintain)

**Recommendation**: Option A - sorting is cheap (1,024 integers), enables O(log n) lookup.

### Q2: LAi Rebuild Frequency
**Question**: How often should we rebuild LAi from scratch?
**Options**:
- A) Only on boot and after SD card operations
- B) Periodic rebuild every 24 hours
- C) Never rebuild, only incremental updates

**Recommendation**: Option A - rebuilds are expensive (stat() every file). Incremental updates are reliable.

### Q3: Stale LAi Entries
**Question**: What if LAi claims a file exists but it was deleted externally (USB mass storage)?
**Options**:
- A) Accept stale entries, rely on playback error to remove them
- B) Validate LAi on every load (expensive)
- C) Validate LAi only when playback fails

**Recommendation**: Option C - file existence check during playback, call `lai_remove_entry()` on failure.

## Pain Points

1. **File Format Migration**: Need to handle old cache files gracefully
2. **SD Card Performance**: stat() calls during LAi rebuild can be slow
3. **Atomic Writes**: Need to ensure Ci+LAi write is atomic (temp file + rename)
4. **Testing**: Need to test LAi with 0, 1, 50%, 100% availability
5. **Web UI Impact**: Web UI file count queries should use `lai_count`, not `entry_count`
