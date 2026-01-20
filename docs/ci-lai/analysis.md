# Ci/LAi Management Analysis

## Executive Summary

This document analyzes the proposed approaches for managing **Ci** (Channel Index) and **LAi** (Locally Available artworks index) in p3a, evaluating current implementation, proposed behaviors, and identifying strengths, weaknesses, edge cases, and alternative approaches.

**Date:** 2026-01-20  
**Component:** `components/channel_manager/`  
**Related Files:** `channel_cache.c`, `makapix_channel_refresh.c`, `download_manager.c`

---

## 1. Current Implementation Overview

### 1.1 Data Structures

**Ci (Channel Index)**
- **Definition:** Array of all known artworks from the channel (`makapix_channel_entry_t *entries`)
- **Maximum Size:** 1,024 entries (`CHANNEL_CACHE_MAX_ENTRIES`)
- **Location:** `channel_cache.h:44-45`
- **Entry Size:** 64 bytes per entry
- **Persistence:** Saved in `.cache` file with CRC32 checksum

**LAi (Locally Available index)**
- **Definition:** Array of indices into Ci for downloaded artworks (`uint32_t *available_indices`)
- **Constraint:** `LAi ⊆ Ci` (subset relationship)
- **Purpose:** O(1) random access for playback without filesystem I/O
- **Location:** `channel_cache.h:80-82`

### 1.2 Key Architectural Components

**Channel Cache System** (`channel_cache.c`)
```c
typedef struct channel_cache_s {
    makapix_channel_entry_t *entries;   // Ci - all known artworks
    size_t entry_count;                 // |Ci|
    uint32_t *available_indices;        // LAi - indices into Ci
    size_t available_count;             // |LAi|
    char channel_id[64];
    bool dirty;
    SemaphoreHandle_t mutex;
} channel_cache_t;
```

**Refresh Task** (`makapix_channel_refresh.c`)
- Queries server in batches via MQTT
- Current batch size: **32 entries** (`query_req.limit = 32`)
- Target total: **1,024 entries** (`TARGET_COUNT = 1024`)
- Eviction batch size: **32 entries** (`EVICTION_BATCH = 32`)

**Download Manager** (`download_manager.c`)
- Round-robin across channels
- Downloads one file at a time
- Uses `channel_cache_get_next_missing()` to find entries not in LAi
- Signals `downloads_needed` after each batch processed

---

## 2. Proposed Behaviors Evaluation

### 2.1 Batch Processing (`bi`)

**Proposed:** Manipulations of Ci should happen in batches matching query_posts batch size.

**Current Implementation:** ✅ **IMPLEMENTED**
- Batch size: 32 entries (line 850 in `makapix_channel_refresh.c`)
- Queries process in batches of 32 until 1,024 total or no more available
- Each batch is committed atomically via `update_index_bin()`

**Analysis:**
- **Strengths:**
  - Reduces server round-trips
  - Atomic updates prevent partial corruption
  - Batch size (32) provides good balance between memory and latency
  
- **Weaknesses:**
  - Batch size hard-coded rather than configurable
  - No adaptive batch sizing based on available memory or network conditions

---

### 2.2 Ci State Validity During Transitions

**Proposed:** Processing `bi` must transition Ci from one valid state to another. Valid state = Not more than 1,024 entries.

**Current Implementation:** ⚠️ **PARTIALLY IMPLEMENTED**

**During Batch Processing:**
```c
// In update_index_bin() (lines 310-576):
1. Load existing Ci entries
2. Merge new batch with existing entries
3. Write atomically to disk
4. Update in-memory Ci
```

**Issue Identified:**
- Ci can temporarily exceed 1,024 entries between batch ingestion and eviction
- Eviction happens AFTER all batches are processed (lines 1001-1002)
- Window where Ci > 1,024 exists

**Example Race Condition:**
```
1. Ci has 1,020 entries
2. Batch of 32 arrives → Ci becomes 1,052 entries
3. Eviction runs → Ci becomes 1,024 entries
```

During step 2-3, Ci is in an invalid state if other tasks access it.

**Recommendation:**
- Move eviction inside `update_index_bin()` to maintain invariant
- OR enforce limit during batch merge (reject excess entries)

---

### 2.3 Sequential Batch Processing

**Proposed:** p3a only queries `bi[n+1]` after `bi[n]` completes processing.

**Current Implementation:** ✅ **IMPLEMENTED**
```c
// makapix_channel_refresh.c lines 894-965
while (total_queried < TARGET_COUNT && ch->refreshing) {
    esp_err_t err = makapix_api_query_posts(&query_req, resp);
    // ... process response ...
    update_index_bin(ch, resp->posts, resp->post_count);
    download_manager_signal_work_available();
    // ... update cursor for next batch ...
    vTaskDelay(pdMS_TO_TICKS(1000));  // Delay between queries
}
```

**Analysis:**
- **Strengths:**
  - Prevents system overload
  - Maintains backpressure
  - 1-second delay provides breathing room
  
- **Weaknesses:**
  - Fixed 1-second delay may be too aggressive or too conservative
  - No feedback mechanism to adjust delay based on system load

---

### 2.4 Handling Duplicates and Updates

**Proposed:** Items in `bi` that already exist in Ci should update metadata without adding duplicates.

**Current Implementation:** ✅ **IMPLEMENTED**
```c
// makapix_channel_refresh.c lines 373-505
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
        // Update existing entry
        all_entries[(size_t)found_idx] = tmp;
    } else {
        // Add new entry
        all_entries[all_count++] = tmp;
    }
}
```

**Analysis:**
- **Strengths:**
  - Prevents duplicates via post_id + kind lookup
  - Updates metadata correctly
  - Detects artwork file changes via `artwork_modified_at` timestamp
  
- **Weaknesses:**
  - O(n²) complexity for duplicate detection (linear search per item)
  - Could use hash table for O(1) lookup with large Ci

---

### 2.5 Eviction When Ci Exceeds 1,024

**Proposed:** 
- Oldest items evicted from Ci when adding new items would exceed 1,024
- Evicting from Ci must trigger eviction from LAi (delete artwork file)
- New items added to Ci ONLY AFTER evictions complete

**Current Implementation:** ⚠️ **PARTIALLY IMPLEMENTED**

**Eviction Process:**
```c
// makapix_channel_refresh.c lines 711-776
esp_err_t evict_excess_artworks(makapix_channel_t *ch, size_t max_count)
{
    // 1. Count downloaded artworks
    // 2. Sort by created_at (oldest first)
    // 3. Delete oldest files in batches of 32
    // 4. Keep index entries (metadata)
}
```

**Critical Issue:** ❌ **DOES NOT DELETE FROM Ci**
- Function only deletes **FILES** (LAi eviction)
- Does **NOT** remove entries from Ci
- Ci can grow unbounded in metadata

**Why This Happens:**
```c
// Line 760-768: Only deletes FILES, not Ci entries
for (size_t i = 0; i < to_delete; i++) {
    char vault_path[512];
    build_vault_path(ch, &downloaded[i], vault_path, sizeof(vault_path));
    if (unlink(vault_path) == 0) {
        actually_deleted++;
    }
}
// No code to remove from ch->entries[]
```

**Recommendation:**
- Implement true Ci eviction that removes entries
- Update LAi to remove evicted Ci indices
- Call eviction BEFORE adding new batch to maintain `|Ci| ≤ 1024` invariant

---

### 2.6 Fill-Availability-Holes Wakeup

**Proposed:** After batch processing, if new items added to Ci, wake fill-availability-holes routine.

**Current Implementation:** ✅ **IMPLEMENTED**
```c
// makapix_channel_refresh.c line 936
download_manager_signal_work_available();
```

Called after every batch ingestion, which signals download_manager to scan for missing files.

**Analysis:**
- **Strengths:**
  - Download manager wakes immediately
  - No polling overhead
  
- **Weaknesses:**
  - Signals even if no new entries added (could optimize)
  - No distinction between "new entries" vs "metadata updates"

---

### 2.7 Parallel Operations

**Proposed:** Channel-refresh and fill-availability-holes run in parallel.

**Current Implementation:** ✅ **IMPLEMENTED**

**Refresh Task:**
- FreeRTOS task: `refresh_task_impl()` (priority 3)
- Continuously queries and updates Ci

**Download Task:**
- FreeRTOS task: `download_task()` (priority 3)
- Scans Ci for items not in LAi
- Downloads one file at a time

**Synchronization:**
- Mutex on `channel_cache_t` for Ci/LAi access
- Event groups for signaling (`downloads_needed`)
- No deadlocks observed in current design

**Analysis:**
- **Strengths:**
  - True parallel execution
  - Proper synchronization primitives
  - Download can start while refresh continues
  
- **Potential Issues:**
  - Both at same priority could cause starvation
  - No priority inversion protection documented

---

### 2.8 Fill-Availability-Holes Implementation

**Proposed:** Routine checks every Ci item for presence in LAi, downloads missing ones one at a time.

**Current Implementation:** ✅ **WELL IMPLEMENTED**

```c
// download_manager.c lines 281-374
static esp_err_t dl_get_next_download(download_request_t *out_request, dl_snapshot_t *snapshot)
{
    // Round-robin across channels
    for (size_t attempt = 0; attempt < snapshot->channel_count; attempt++) {
        // Get channel cache
        channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
        
        // Find entries needing download (artwork not in LAi)
        makapix_channel_entry_t entry;
        while (channel_cache_get_next_missing(cache, &ch->dl_cursor, &entry) == ESP_OK) {
            // Skip if 404 marker exists
            // Skip if LTF (load-to-failure) terminal
            // Found entry needing download
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
```

**`channel_cache_get_next_missing()` Implementation:**
```c
// channel_cache.c lines 1002-1046
esp_err_t channel_cache_get_next_missing(channel_cache_t *cache,
                                          uint32_t *cursor,
                                          makapix_channel_entry_t *out_entry)
{
    // Scan from cursor position
    // Find next artwork entry not in LAi
    // Advance cursor
    // Return entry
}
```

**Analysis:**
- **Strengths:**
  - O(1) amortized scan via cursor
  - Skips playlists automatically
  - Respects 404 markers and LTF tracking
  - Round-robin ensures fairness across channels
  
- **Edge Cases Handled:**
  - Empty Ci: returns ESP_ERR_NOT_FOUND
  - All files downloaded: cursor wraps, returns NOT_FOUND
  - Concurrent Ci updates: mutex-protected reads

---

## 3. Edge Cases and Concerns

### 3.1 Race Conditions

**Scenario: Concurrent Ci Update and Download**
```
Thread 1 (Refresh):           Thread 2 (Download):
Take mutex                    
Add entry at index 500        
Release mutex                 
                              Take mutex
                              Read entry 500
                              Release mutex
                              Download file
                              Take mutex
Thread 1 (Refresh):           Add to LAi at index 500
Take mutex                    Release mutex
Evict entry 500               
Release mutex                 
```

**Result:** LAi references evicted Ci entry → undefined behavior

**Mitigation:** 
- Current: No mitigation observed
- Recommendation: Reference counting or tombstones for in-flight downloads

---

### 3.2 Cursor Invalidation

**Scenario: Ci Shrinks During Scan**
```
1. Download cursor at index 800
2. Ci has 900 entries
3. Eviction removes entries 0-200
4. Ci now has 700 entries
5. Cursor 800 now out of bounds
```

**Current Handling:**
```c
// channel_cache.c line 1018
if (ci_index >= cache->entry_count) {
    return ESP_ERR_NOT_FOUND;  // Safe: returns end-of-scan
}
```

**Analysis:** ✅ Handled safely by bounds check

---

### 3.3 Reconciliation vs. Eviction Conflict

**Scenario: Server Deletes Item That Was Just Downloaded**
```
1. Refresh queries batch with post_id=123
2. Download starts for post_id=123
3. Server deletes post_id=123 before next query
4. Reconciliation removes Ci entry for 123
5. Download completes for non-existent Ci entry
```

**Current Handling:**
```c
// makapix_channel_refresh.c lines 122-194: reconcile_deletions()
// Removes Ci entries not in server response
// Deletes local files for removed entries
```

**Issue:** ❌ No coordination with in-flight downloads

**Recommendation:** 
- Mark entries as "delete-pending" rather than immediate removal
- Download manager checks delete-pending flag before completing

---

### 3.4 Storage Exhaustion

**Scenario: SD Card Fills Mid-Download**
```
1. Ci has 500 entries, LAi has 100
2. Download manager starts filling holes
3. SD card fills after 50 more downloads
4. Remaining 350 entries can never be downloaded
```

**Current Handling:**
```c
// makapix_channel_refresh.c lines 623-709: evict_for_storage_pressure()
// Evicts oldest files when free space < 10MB
```

**Analysis:**
- ⚠️ Storage pressure eviction runs ONLY during refresh, not during downloads
- ⚠️ `get_storage_free_space()` returns `UINT64_MAX` (unimplemented)
- Recommendation: Implement statvfs and run eviction before each download

---

### 3.5 Batch Size Mismatch

**Scenario: Server Changes Batch Size**
```
1. p3a queries with limit=32
2. Server returns 64 entries (ignoring limit)
3. Ci could exceed 1,024 if not careful
```

**Current Handling:**
```c
// Unclear if server honors limit parameter
// No defensive cap on batch size in update_index_bin()
```

**Recommendation:** 
- Cap batch size: `count = min(count, CHANNEL_CACHE_MAX_ENTRIES - ch->entry_count)`
- Validate server response size

---

## 4. Strengths of Current Approach

### 4.1 Architecture

✅ **Clean Separation of Concerns**
- Channel cache: Data persistence
- Refresh task: Server synchronization
- Download manager: File acquisition
- Each component has well-defined responsibilities

✅ **Efficient Data Structures**
- LAi provides O(1) random access for playback
- Cursor-based scanning avoids O(n) searches
- Mutex-protected shared state

✅ **Graceful Degradation**
- Works with partial Ci/LAi
- Handles empty states
- 404 markers prevent retry loops

### 4.2 Synchronization

✅ **Event-Driven Design**
- Event groups prevent busy-waiting
- Signal-based wakeup conserves CPU
- Proper use of FreeRTOS primitives

✅ **Atomic Persistence**
- .tmp file + rename ensures crash safety
- CRC32 detects corruption
- Legacy format migration

### 4.3 Robustness

✅ **Load-to-Failure Tracking**
- Prevents infinite retry of broken files
- Terminal failure after 3 attempts
- Per-file tracking

✅ **Round-Robin Fairness**
- Multiple channels get equal download bandwidth
- No channel starvation

---

## 5. Weaknesses and Gaps

### 5.1 Critical Issues

❌ **Ci Eviction Not Implemented**
- `evict_excess_artworks()` only deletes files, not Ci entries
- Ci can grow unbounded in metadata
- Violates 1,024 entry limit

❌ **No Eviction-Before-Add**
- New entries added before eviction
- Temporary Ci > 1,024 entries possible
- Violates state invariant

❌ **Race Condition: Download vs. Eviction**
- In-flight downloads can target evicted Ci entries
- No reference counting or coordination

### 5.2 Performance Issues

⚠️ **O(n²) Duplicate Detection**
- Linear search for each batch item
- Inefficient for large Ci
- Could use hash table

⚠️ **Fixed Batch Delay**
- 1-second delay between queries hard-coded
- No adaptive adjustment
- Could be too slow or too fast

### 5.3 Missing Features

⚠️ **Storage Pressure Detection Unimplemented**
- `get_storage_free_space()` always returns `UINT64_MAX`
- Eviction for storage pressure ineffective
- Relies solely on count-based eviction

⚠️ **No Cursor Reset on Ci Shrink**
- Download cursor can point past end after eviction
- Handled safely but inefficiently (stops scanning)
- Could reset cursor to 0 on eviction

---

## 6. Alternative Approaches

### 6.1 Alternative: Pre-Eviction Before Batch Add

**Current:**
```
1. Add batch to Ci
2. Evict if Ci > 1,024
```

**Alternative:**
```
1. Check if |Ci| + |batch| > 1,024
2. Evict (|Ci| + |batch| - 1,024) entries
3. Add batch to Ci
```

**Pros:**
- Maintains invariant at all times
- No temporary overage

**Cons:**
- Slightly more complex logic
- May evict more than necessary if batch has duplicates

---

### 6.2 Alternative: Circular Buffer Ci

**Current:** Dynamic array with eviction

**Alternative:** Fixed 1,024-entry circular buffer
```c
typedef struct {
    makapix_channel_entry_t entries[1024];
    size_t head;  // Oldest entry
    size_t tail;  // Newest entry
    size_t count; // Current count
} circular_ci_t;
```

**Pros:**
- O(1) add (overwrite oldest)
- No realloc needed
- Guaranteed size limit

**Cons:**
- Cannot grow beyond 1,024
- More complex index arithmetic
- LAi indices become more complex (need modulo)

---

### 6.3 Alternative: Lazy Eviction

**Current:** Eager eviction after refresh cycle

**Alternative:** Evict only when download manager needs space
```c
// In download_manager before downloading:
if (storage_low() || ci_full()) {
    evict_one_entry();
}
```

**Pros:**
- Evicts only when necessary
- Better cache hit rate (keeps more metadata)

**Cons:**
- Download latency increases
- More complex eviction policy needed
- Risk of evicting recently added items

---

### 6.4 Alternative: Two-Level Ci (Hot/Cold)

**Concept:**
- Hot Ci: Recently added/accessed (512 entries)
- Cold Ci: Older entries (512 entries)
- Downloads prioritize Hot Ci
- Eviction always from Cold Ci

**Pros:**
- Better cache locality
- Prevents evicting recently added items
- LRU-like behavior

**Cons:**
- More complex management
- Need to track access patterns
- Migration between hot/cold adds overhead

---

### 6.5 Alternative: Reference Counting for In-Flight Downloads

**Concept:**
```c
typedef struct {
    makapix_channel_entry_t entry;
    uint32_t refcount;  // 0 = can evict, >0 = in use
} refcounted_entry_t;
```

**Pros:**
- Prevents eviction of in-flight downloads
- Solves race condition cleanly
- Standard pattern

**Cons:**
- Extra memory per entry (4 bytes)
- Complexity in increment/decrement
- Need to handle leaks (stuck refcount)

---

## 7. Recommendations

### 7.1 Immediate Fixes (Critical)

1. **Implement True Ci Eviction**
   - Modify `evict_excess_artworks()` to remove Ci entries
   - Update LAi indices after Ci shrinks
   - Test: Ci never exceeds 1,024 entries

2. **Pre-Evict Before Batch Add**
   - Move eviction before `update_index_bin()`
   - Calculate: `to_evict = max(0, |Ci| + |batch| - 1024)`
   - Ensures Ci invariant maintained

3. **Add Download Reference Counting**
   - Mark Ci entries as "in-flight" during download
   - Skip in-flight entries during eviction
   - Clear flag on download complete/fail

### 7.2 Short-Term Improvements

4. **Implement Storage Pressure Detection**
   - Use ESP-IDF VFS to get free space
   - Set threshold: min_free = 10MB
   - Evict before each download if below threshold

5. **Optimize Duplicate Detection**
   - Use hash table for O(1) post_id lookup
   - ESP-IDF has `uthash` available
   - Reduces batch processing from O(n²) to O(n)

6. **Add Cursor Reset on Eviction**
   - Reset download cursor to 0 after Ci shrinks
   - Prevents missing newly available files
   - Minimal change, high impact

### 7.3 Long-Term Enhancements

7. **Adaptive Batch Delay**
   - Monitor system load (CPU, heap, SDIO contention)
   - Adjust delay between queries: 500ms to 5s
   - Use exponential backoff on errors

8. **Eviction Policy Improvements**
   - Consider LRU instead of pure FIFO
   - Track last_played_at timestamp
   - Evict least-recently-played first

9. **Cross-Channel Eviction**
   - When storage critical, evict across all channels
   - Load all channel indices temporarily
   - Global LRU across channels

---

## 8. Conclusion

### 8.1 Are the Proposed Approaches Sound?

**Mostly Yes,** with caveats:

✅ Batch processing (32 entries) is appropriate  
✅ Sequential batch queries prevent overload  
✅ Parallel refresh + download is correct design  
✅ Fill-availability-holes implementation is solid  
❌ **Critical:** Ci eviction is incomplete (only deletes files, not entries)  
❌ **Critical:** Race condition between download and eviction unhandled  

### 8.2 Important Edge Cases to Address

1. **In-flight downloads targeting evicted Ci entries** (race condition)
2. **Ci temporarily exceeding 1,024 during batch add** (invariant violation)
3. **Cursor invalidation when Ci shrinks** (handled but inefficiently)
4. **Storage exhaustion not detected** (unimplemented statvfs)
5. **O(n²) duplicate detection** (performance issue at scale)

### 8.3 Overall Assessment

The architecture is fundamentally sound with good separation of concerns and proper use of FreeRTOS primitives. However, the **incomplete Ci eviction implementation** is a critical bug that must be fixed. The system will work correctly for the initial 1,024 entries but will fail to maintain the invariant after that.

**Risk Level:** 🔴 **HIGH** - Data structure invariants violated  
**Code Quality:** 🟡 **MODERATE** - Good design, incomplete implementation  
**Recommendation:** **Fix critical issues before deployment**

---

## Appendix A: Key Code Locations

| Feature | File | Line(s) |
|---------|------|---------|
| Ci/LAi Structures | `channel_cache.h` | 75-91 |
| Batch Query | `makapix_channel_refresh.c` | 850, 894-965 |
| Update Index | `makapix_channel_refresh.c` | 310-576 |
| Eviction (Files Only) | `makapix_channel_refresh.c` | 711-776 |
| Get Next Missing | `channel_cache.c` | 1002-1046 |
| Download Manager | `download_manager.c` | 281-374, 386-564 |
| LAi Operations | `channel_cache.c` | 645-741 |

---

## Appendix B: Invariants to Maintain

1. **Ci Size:** `|Ci| ≤ 1024` at all observable states
2. **LAi Subset:** `LAi ⊆ Ci` (all LAi indices valid in Ci)
3. **No Duplicates:** `∀ i,j: i≠j ⇒ Ci[i].post_id ≠ Ci[j].post_id ∨ Ci[i].kind ≠ Ci[j].kind`
4. **LAi Files Exist:** `∀ idx ∈ LAi: file_exists(vault_path(Ci[idx]))`
5. **Cursor Bounds:** `cursor ≤ |Ci|` (end-of-scan allowed)

---

*Document Version: 1.0*  
*Author: Code Analysis Agent*  
*Review Status: Draft*
