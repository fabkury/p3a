# Proposed Ci/LAi Behaviors - Compliance Analysis

This document evaluates each behavior proposed in the problem statement against the current implementation.

## Behavior 1: Batch Size Matching

**Proposed:**
> Manipulations of Ci should happen in batches called `bi`. The size of the batch should match the batch size used for query_posts queries.

**Status:** ✅ **COMPLIANT**

**Evidence:**
```c
// makapix_channel_refresh.c:850
query_req.limit = 32;

// Each query returns up to 32 entries, processed together in update_index_bin()
```

**Implementation Quality:** Good
- Batch size is consistent (32 entries)
- All entries from a query batch are processed atomically
- Reduces network round-trips

**Potential Improvement:**
- Make batch size configurable via Kconfig or NVS
- Consider adaptive batch sizing based on available memory

---

## Behavior 2: Valid State Transitions

**Proposed:**
> The processing of `bi`, a batch of entries from the server to be ingested to Ci, must transition Ci from one valid state to another. Valid Ci state = Not more than 1,024 entries. Remember that Ci might be being accessed by other parallel tasks simultaneously in the system (race conditions must be prevented).

**Status:** ❌ **NON-COMPLIANT**

**Issues Found:**

1. **Temporary Overage:** Ci can exceed 1,024 between batch add and eviction
   ```c
   // Sequence in refresh_task_impl():
   update_index_bin(ch, resp->posts, resp->post_count);  // Ci = 1,052
   // ... later ...
   evict_excess_artworks(ch, TARGET_COUNT);              // Ci = 1,024
   ```

2. **Incomplete Eviction:** `evict_excess_artworks()` only deletes files, not Ci entries
   ```c
   // Line 760-768: Only unlinks files, ch->entries[] unchanged
   for (size_t i = 0; i < to_delete; i++) {
       unlink(vault_path);  // File deleted
       // Missing: Remove from ch->entries[]
   }
   ```

3. **Race Conditions:** Mutex protection exists but not sufficient
   - Ci can be read between batch add and eviction
   - Download manager can target entries about to be evicted
   - No reference counting for in-flight operations

**Fix Required:** HIGH PRIORITY
- Evict BEFORE adding batch to maintain `|Ci| ≤ 1024`
- Implement true Ci entry removal in `evict_excess_artworks()`
- Add reference counting or tombstones for concurrent access

---

## Behavior 3: Sequential Batch Processing

**Proposed:**
> p3a only queries the next batch `bi[n+1]` from the server after the previous batch `bi[n]` has completed processing. This is to avoid overloading the system with batches to process.

**Status:** ✅ **COMPLIANT**

**Evidence:**
```c
// makapix_channel_refresh.c:894-965
while (total_queried < TARGET_COUNT && ch->refreshing) {
    // 1. Query batch
    esp_err_t err = makapix_api_query_posts(&query_req, resp);
    
    // 2. Wait for response (blocking call)
    
    // 3. Process batch completely
    update_index_bin(ch, resp->posts, resp->post_count);
    
    // 4. Signal downloads (non-blocking)
    download_manager_signal_work_available();
    
    // 5. Delay before next query
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

**Implementation Quality:** Excellent
- Strictly sequential processing
- Backpressure maintained
- 1-second delay between queries provides breathing room

**Potential Improvement:**
- Make delay adaptive based on system load
- Add exponential backoff on errors

---

## Behavior 4: Handling Existing Items

**Proposed:**
> When processing `bi`, some or all of the items in `bi` already exist in Ci:
> - The metadata of these items gets updated
> - No duplicate entries are added to Ci

**Status:** ✅ **COMPLIANT**

**Evidence:**
```c
// makapix_channel_refresh.c:373-505
for (size_t i = 0; i < count; i++) {
    const makapix_post_t *post = &posts[i];
    
    // Find existing entry by (post_id, kind)
    int found_idx = -1;
    for (size_t j = 0; j < all_count; j++) {
        if (all_entries[j].post_id == post->post_id && 
            all_entries[j].kind == (uint8_t)post->kind) {
            found_idx = (int)j;
            break;
        }
    }
    
    if (found_idx >= 0) {
        // Update existing - metadata refreshed
        all_entries[(size_t)found_idx] = tmp;
    } else {
        // Add new - no duplicate created
        all_entries[all_count++] = tmp;
    }
}
```

**Implementation Quality:** Good
- Correctly prevents duplicates via (post_id, kind) composite key
- Metadata properly updated on existing entries
- Detects artwork file changes via timestamp

**Potential Improvement:**
- O(n²) complexity: Use hash table for O(1) lookup
- Consider using Bloom filter for fast "probably new" check

---

## Behavior 5: Eviction on Overflow

**Proposed:**
> When adding the new items from `bi` causes Ci to exceed 1,024 entries:
> - Oldest items need to be evicted from Ci
> - Evicting from Ci must trigger eviction from LAi too -- i.e. artwork file itself gets deleted
> - New items from `bi` are added to Ci only AFTER any necessary evictions are completed

**Status:** ❌ **NON-COMPLIANT**

**Issues Found:**

1. **Eviction Timing:** Eviction happens AFTER batch add, not before
   ```c
   // Current sequence (wrong):
   update_index_bin(ch, resp->posts, resp->post_count);  // Add first
   evict_excess_artworks(ch, TARGET_COUNT);              // Evict after
   
   // Should be:
   evict_excess_artworks(ch, TARGET_COUNT - resp->post_count);  // Evict first
   update_index_bin(ch, resp->posts, resp->post_count);         // Add after
   ```

2. **Incomplete Ci Eviction:** Only files deleted, not Ci entries
   ```c
   // evict_excess_artworks() line 760-768:
   unlink(vault_path);  // ✓ File deleted (LAi eviction)
   // Missing: Remove from ch->entries[] (Ci eviction)
   ```

3. **LAi Not Updated:** No code to remove evicted Ci indices from LAi
   - LAi indices become invalid after Ci shrinks
   - Can cause crashes when accessing LAi[i] where LAi[i] >= |Ci|

**Fix Required:** CRITICAL PRIORITY
- Reorder: evict → add (not add → evict)
- Implement Ci entry removal
- Update LAi after Ci eviction (rebuild or adjust indices)

---

## Behavior 6: Wake Fill-Availability-Holes

**Proposed:**
> If any new items from `bi` were actually added to Ci, after completing processing the batch, the procedure fill-availability-holes is woken (if it is not already up) to check for any file availability hole

**Status:** ⚠️ **PARTIALLY COMPLIANT**

**Evidence:**
```c
// makapix_channel_refresh.c:936
download_manager_signal_work_available();
```

**What Works:**
- Download manager is signaled after every batch
- Signal wakes sleeping download task
- No polling overhead

**Issues:**
1. **Signals Even When No New Entries:** Wastes wakeups on metadata-only updates
   ```c
   // Should be:
   if (new_entries_added > 0) {
       download_manager_signal_work_available();
   }
   ```

2. **No "Already Running" Check:** Signal sent even if download task is busy
   - Not a bug (signal is idempotent) but could be optimized

**Improvement Needed:** LOW PRIORITY
- Track if new entries were added vs. just metadata updates
- Only signal when actually needed

---

## Behavior 7: Fill-Availability-Holes Definition

**Proposed:**
> - File availability hole = entry in Ci that is not yet in LAi
> - fill-availability-holes: routine that checks every item in Ci if it is present in LAi. If not present, requests a download of each file, one at a time.

**Status:** ✅ **COMPLIANT**

**Evidence:**
```c
// channel_cache.c:1002-1046
esp_err_t channel_cache_get_next_missing(channel_cache_t *cache,
                                          uint32_t *cursor,
                                          makapix_channel_entry_t *out_entry)
{
    // Scan Ci from cursor position
    for (uint32_t ci_index = *cursor; ci_index < cache->entry_count; ci_index++) {
        const makapix_channel_entry_t *entry = &cache->entries[ci_index];
        
        // Skip playlists (no direct files)
        if (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) {
            continue;
        }
        
        // Check if in LAi
        if (lai_contains(cache, ci_index)) {
            continue;  // Already downloaded
        }
        
        // Found hole: entry in Ci, not in LAi
        *out_entry = *entry;
        *cursor = ci_index + 1;
        return ESP_OK;
    }
    
    return ESP_ERR_NOT_FOUND;  // No more holes
}
```

**Implementation Quality:** Excellent
- Correctly identifies Ci entries not in LAi
- Cursor-based iteration is efficient
- Skips playlists automatically
- Downloads one at a time (see download_task)

**No Improvements Needed**

---

## Behavior 8: Parallel Execution

**Proposed:**
> These two actions need to happen in parallel:
> - Channel-refresh: Query the server in batches, process received `bi`'s, wake up fill-availability-holes every time new items are actually added to Ci.
> - fill-availability-holes: Scan Ci for items not in LAi, download them one by one (only 1 download at a time) until the routine is interrupted or there are no more file availability holes.

**Status:** ✅ **COMPLIANT**

**Evidence:**

**Refresh Task:**
```c
// makapix_channel_refresh.c:778-1055
void refresh_task_impl(void *pvParameters)
{
    // Runs continuously in background
    while (ch->refreshing) {
        // Query batches
        // Update Ci
        // Signal downloads
        // Sleep between cycles
    }
}
```

**Download Task:**
```c
// download_manager.c:386-564
static void download_task(void *arg)
{
    // Runs continuously in parallel
    while (true) {
        // Get next missing file
        dl_get_next_download(&s_dl_req, &s_dl_snapshot);
        
        // Download ONE file
        makapix_artwork_download(...);
        
        // Update LAi
        play_scheduler_on_download_complete(...);
    }
}
```

**Synchronization:**
- Both use FreeRTOS tasks (true parallelism)
- Mutex on `channel_cache_t` for Ci/LAi access
- Event groups for signaling (no busy-wait)
- No deadlocks observed

**Implementation Quality:** Excellent
- Clean separation of concerns
- Proper use of FreeRTOS primitives
- One download at a time enforced
- Round-robin fairness across multiple channels

**No Improvements Needed** for basic functionality

**Potential Enhancement:**
- Priority-based download (recently added items first)
- Configurable max concurrent downloads (currently fixed at 1)

---

## Summary Scorecard

| Behavior | Status | Priority | Notes |
|----------|--------|----------|-------|
| 1. Batch Size Matching | ✅ Compliant | - | Working correctly |
| 2. Valid State Transitions | ❌ Non-Compliant | 🔴 CRITICAL | Ci can exceed 1,024; eviction incomplete |
| 3. Sequential Processing | ✅ Compliant | - | Working correctly |
| 4. Duplicate Handling | ✅ Compliant | - | Working correctly (O(n²) perf issue) |
| 5. Eviction on Overflow | ❌ Non-Compliant | 🔴 CRITICAL | Wrong order; Ci eviction missing |
| 6. Wake Downloads | ⚠️ Partial | 🟡 LOW | Works but could optimize |
| 7. Hole Definition | ✅ Compliant | - | Working correctly |
| 8. Parallel Execution | ✅ Compliant | - | Working correctly |

**Overall Compliance:** 5/8 Compliant, 2/8 Non-Compliant (Critical), 1/8 Partial

**Risk Assessment:** 🔴 **HIGH** - Two critical issues must be fixed before production use.

---

## Action Items

### Immediate (Before Release)
1. ✅ Fix Behavior 2: Implement true Ci eviction
2. ✅ Fix Behavior 5: Reorder eviction before batch add
3. ⚠️ Add reference counting for in-flight downloads

### Short-Term (Next Sprint)
4. 🟡 Optimize Behavior 4: Hash table for O(1) duplicate detection
5. 🟡 Improve Behavior 6: Only signal when new entries added
6. 🟡 Implement storage pressure detection (currently stubbed)

### Long-Term (Future Enhancements)
7. 🟢 Adaptive batch sizing
8. 🟢 LRU eviction policy
9. 🟢 Cross-channel eviction
10. 🟢 Configurable max concurrent downloads

---

*Document Version: 1.0*  
*Compliance Level: 62.5% (5/8)*  
*Critical Issues: 2*  
*Recommendation: FIX CRITICAL ISSUES BEFORE RELEASE*
