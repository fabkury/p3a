# Ci/LAi Management: Implementation Checklist

## Critical Fixes Required

### 1. Fix Ci Eviction (CRITICAL - Data Corruption Risk)

**Current Problem:** `evict_excess_artworks()` only deletes files, not Ci entries. Ci can grow unbounded.

**Files to Modify:**
- `components/channel_manager/makapix_channel_refresh.c`

**Changes Needed:**
```c
// In evict_excess_artworks() around line 760:
// CURRENT (wrong):
for (size_t i = 0; i < to_delete; i++) {
    char vault_path[512];
    build_vault_path(ch, &downloaded[i], vault_path, sizeof(vault_path));
    if (unlink(vault_path) == 0) {
        actually_deleted++;
    }
}

// SHOULD BE:
// 1. Collect post_ids of entries to evict
int32_t *evicted_post_ids = malloc(to_delete * sizeof(int32_t));
for (size_t i = 0; i < to_delete; i++) {
    evicted_post_ids[i] = downloaded[i].post_id;
    char vault_path[512];
    build_vault_path(ch, &downloaded[i], vault_path, sizeof(vault_path));
    if (unlink(vault_path) == 0) {
        actually_deleted++;
    }
}

// 2. Remove evicted entries from Ci
size_t new_count = 0;
for (size_t i = 0; i < ch->entry_count; i++) {
    bool should_evict = false;
    for (size_t j = 0; j < to_delete; j++) {
        if (ch->entries[i].post_id == evicted_post_ids[j]) {
            should_evict = true;
            break;
        }
    }
    if (!should_evict) {
        ch->entries[new_count++] = ch->entries[i];
    }
}
ch->entry_count = new_count;

// 3. Update LAi to remove references to evicted Ci indices
// (LAi indices may now be invalid - need to rebuild or adjust)
// Simplest: rebuild LAi from scratch
// More efficient: adjust indices (complex)

free(evicted_post_ids);
```

**Testing:**
- Add 1,100 entries via multiple batches
- Verify Ci never exceeds 1,024 entries
- Verify LAi indices remain valid

---

### 2. Pre-Evict Before Batch Add (CRITICAL - Invariant Violation)

**Current Problem:** New entries added before eviction, Ci temporarily exceeds 1,024.

**Files to Modify:**
- `components/channel_manager/makapix_channel_refresh.c`

**Changes Needed:**
```c
// In refresh_task_impl() around line 925, BEFORE update_index_bin():

// Calculate how many entries we'll have after this batch
size_t entries_after_batch = ch->entry_count + resp->post_count;

// If would exceed limit, pre-evict
if (entries_after_batch > TARGET_COUNT) {
    size_t to_evict = entries_after_batch - TARGET_COUNT;
    // Evict in batches of 32
    to_evict = ((to_evict + 31) / 32) * 32;
    
    // Pre-evict BEFORE adding batch
    evict_excess_artworks(ch, TARGET_COUNT - to_evict);
}

// NOW safe to add batch
update_index_bin(ch, resp->posts, resp->post_count);
```

**Testing:**
- Monitor Ci size during batch processing
- Verify Ci never exceeds 1,024 at any point

---

### 3. Prevent Download vs. Eviction Race (HIGH PRIORITY)

**Current Problem:** Download manager can target Ci entries that get evicted mid-download.

**Solution Option A: Reference Counting (Recommended)**

**Files to Modify:**
- `components/channel_manager/include/makapix_channel_impl.h`
- `components/channel_manager/makapix_channel_refresh.c`
- `components/channel_manager/download_manager.c`

**Changes:**
```c
// 1. Add refcount to entry structure:
typedef struct {
    // ... existing fields ...
    uint32_t refcount;  // Number of active references (downloads in flight)
} makapix_channel_entry_t;

// 2. In download_manager before starting download:
// Find Ci entry and increment refcount

// 3. In evict_excess_artworks():
// Skip entries with refcount > 0

// 4. In download_manager after download complete/fail:
// Decrement refcount
```

**Solution Option B: Download Queue (Simpler)**

**Alternative:** Queue download requests by post_id, check Ci exists before starting each download.

```c
// In download_manager, before downloading:
if (!ci_entry_exists(cache, post_id)) {
    ESP_LOGW(TAG, "Entry evicted before download, skipping");
    continue;
}
```

---

## Performance Optimizations

### 4. O(n²) → O(1) Duplicate Detection (MEDIUM PRIORITY)

**Current Problem:** Linear search for duplicates during batch processing.

**Files to Modify:**
- `components/channel_manager/makapix_channel_refresh.c`

**Solution:** Use hash table for post_id → index mapping.

**Changes:**
```c
// Use ESP-IDF component: kvstore or implement simple hash table
// Example using array-based hash (simpler than full hash table):

typedef struct {
    int32_t post_id;
    uint8_t kind;
    size_t ci_index;
} entry_lookup_t;

// Build lookup table before batch processing
entry_lookup_t *lookup = malloc(ch->entry_count * sizeof(entry_lookup_t));
for (size_t i = 0; i < ch->entry_count; i++) {
    lookup[i].post_id = ch->entries[i].post_id;
    lookup[i].kind = ch->entries[i].kind;
    lookup[i].ci_index = i;
}

// Sort by post_id for binary search
qsort(lookup, ch->entry_count, sizeof(entry_lookup_t), compare_by_post_id);

// In batch loop, use binary search instead of linear:
int found_idx = bsearch_lookup(lookup, ch->entry_count, post->post_id, post->kind);
```

**Expected Improvement:** Batch processing time reduces from O(n²) to O(n log n).

---

### 5. Implement Storage Pressure Detection (MEDIUM PRIORITY)

**Current Problem:** `get_storage_free_space()` returns UINT64_MAX (unimplemented).

**Files to Modify:**
- `components/channel_manager/makapix_channel_refresh.c`

**Changes:**
```c
static esp_err_t get_storage_free_space(const char *path, uint64_t *out_free_bytes)
{
    // Use FATFS-specific API for ESP32
    FATFS *fs;
    DWORD free_clusters;
    
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) {
        return ESP_FAIL;
    }
    
    uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors = free_clusters * fs->csize;
    uint64_t sector_size = FF_MAX_SS;  // Or fs->ssize if available
    
    *out_free_bytes = free_sectors * sector_size;
    return ESP_OK;
}
```

**Testing:**
- Fill SD card to near capacity
- Verify eviction triggers when free space < 10MB
- Verify system doesn't crash on full disk

---

### 6. Reset Download Cursor on Ci Shrink (LOW PRIORITY)

**Current Problem:** Cursor can point past end after eviction, stops scanning.

**Files to Modify:**
- `components/channel_manager/download_manager.c`

**Changes:**
```c
// In dl_get_next_download(), after eviction detected:
if (ch->dl_cursor >= cache->entry_count) {
    ch->dl_cursor = 0;  // Reset to start
    // Mark channel as incomplete to re-scan
    ch->channel_complete = false;
}
```

---

## Testing Checklist

### Unit Tests Needed

- [ ] Test Ci eviction removes entries
- [ ] Test LAi indices remain valid after Ci eviction
- [ ] Test Ci never exceeds 1,024 entries during batch add
- [ ] Test duplicate detection (same post_id updates, not adds)
- [ ] Test cursor bounds check when Ci shrinks
- [ ] Test storage pressure eviction (mock filesystem)
- [ ] Test download vs. eviction race (multi-threaded)

### Integration Tests Needed

- [ ] Test full refresh cycle: 0 → 1,024 entries
- [ ] Test refresh with updates (metadata changes)
- [ ] Test refresh with deletions (reconciliation)
- [ ] Test parallel refresh + download
- [ ] Test storage exhaustion scenario
- [ ] Test network failures during refresh
- [ ] Test corruption recovery (CRC32 mismatch)

### System Tests Needed

- [ ] Run for 24 hours with continuous refresh cycles
- [ ] Monitor heap fragmentation
- [ ] Monitor Ci/LAi consistency
- [ ] Verify no memory leaks
- [ ] Verify no deadlocks or livelocks

---

## Configuration Recommendations

### Tunable Parameters

```c
// In channel_cache.h or Kconfig:
#define CHANNEL_CACHE_MAX_ENTRIES     1024    // Current
#define CHANNEL_CACHE_MIN_FREE_BYTES  (10*1024*1024)  // 10MB
#define CHANNEL_REFRESH_BATCH_SIZE    32      // Query batch size
#define CHANNEL_EVICTION_BATCH_SIZE   32      // Eviction batch size
#define CHANNEL_REFRESH_INTERVAL_SEC  3600    // 1 hour
#define CHANNEL_QUERY_DELAY_MS        1000    // Between queries
```

Consider making these runtime-configurable via NVS for testing/tuning.

---

## Future Enhancements

### Phase 2 (Post-Critical Fixes)

1. **LRU Eviction Policy**
   - Track last_played_at timestamp
   - Evict least-recently-played instead of oldest
   - Requires additional 4 bytes per entry

2. **Adaptive Batch Delay**
   - Monitor system load (heap free, CPU usage)
   - Adjust query delay: 500ms to 5s
   - Use exponential backoff on errors

3. **Cross-Channel Eviction**
   - When storage critical, evict across all channels
   - Requires loading all channel indices
   - Global LRU ordering

4. **Incremental Ci Updates**
   - Use delta updates instead of full Ci writes
   - Reduces write amplification on SD card
   - Requires journal or log-structured format

5. **Bloom Filter for LAi**
   - Probabilistic "is downloaded?" check
   - Reduces cache misses during playback
   - Only 1-2KB overhead

---

## Sign-Off Checklist

Before marking this issue as complete:

- [ ] All CRITICAL fixes implemented
- [ ] Unit tests pass
- [ ] Integration tests pass
- [ ] Code reviewed by senior developer
- [ ] Performance profiling shows no regressions
- [ ] Documentation updated
- [ ] Heap usage monitored under load
- [ ] SD card write patterns analyzed
- [ ] No new compiler warnings
- [ ] Release notes updated

---

*Document Version: 1.0*  
*Priority: CRITICAL*  
*Estimated Effort: 3-5 days*
