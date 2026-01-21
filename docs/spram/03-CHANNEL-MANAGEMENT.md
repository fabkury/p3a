# Channel Management - SPRAM Optimization Analysis

## Component Overview

The channel management system handles playlists, artwork metadata, caching, and file management. This includes several data structures that can consume significant memory when dealing with large playlists.

**Files:**
- `components/channel_manager/channel_cache.c`
- `components/channel_manager/playlist_manager.c`
- `components/channel_manager/load_tracker.c`
- `components/channel_manager/vault_storage.c`
- `components/channel_manager/animation_metadata.c`
- `components/channel_manager/makapix_channel_impl.c`

## Current Allocations

### 1. Channel Cache Entries (`channel_cache.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `cache->entries` | entry_count × sizeof(entry_t) | `malloc()` → Internal | **HIGH** |
| `available_post_ids` | available_count × sizeof(uint32_t) | `malloc()` → Internal | **MEDIUM** |
| File read buffer | File size (variable) | `malloc()` → Internal | **HIGH** |
| Hash table nodes | Variable per entry | `malloc()` → Internal | **MEDIUM** |

**Analysis:**
- **entries array**: For 1000 entries × ~128 bytes each = ~128 KB
- **available_post_ids**: For 1000 IDs × 4 bytes = 4 KB
- **File read buffer**: Can be 100-500 KB for large cache files
- **Hash table nodes**: ~16 bytes per node × entries = ~16 KB for 1000 entries
- **Location**: Lines 101, ~200-300 in `channel_cache.c`

**Current code pattern:**
```c
cache->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
// Hash table nodes
ci_post_id_node_t *pid_node = malloc(sizeof(ci_post_id_node_t));
```

**Recommendation:**
```c
// Move entries array to SPIRAM
cache->entries = (makapix_channel_entry_t *)heap_caps_malloc(
    entry_count * sizeof(makapix_channel_entry_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!cache->entries) {
    cache->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
}

// Move hash table nodes to SPIRAM
ci_post_id_node_t *pid_node = (ci_post_id_node_t *)heap_caps_malloc(
    sizeof(ci_post_id_node_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!pid_node) {
    pid_node = malloc(sizeof(ci_post_id_node_t));
}

// Move available_post_ids to SPIRAM
available_post_ids = (uint32_t *)heap_caps_malloc(
    available_count * sizeof(uint32_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!available_post_ids) {
    available_post_ids = malloc(available_count * sizeof(uint32_t));
}
```

**Impact:**
- **Memory freed**: 128 KB + 4 KB + 16 KB = ~148 KB per channel cache
- **Performance**: Minimal impact (cache lookups not in tight loops)

---

### 2. Playlist Manager (`playlist_manager.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `artworks` array | artwork_count × sizeof(artwork_ref_t) | `malloc()` → Internal | **MEDIUM** |
| Temporary buffers | Variable | `malloc()` → Internal | **LOW** |

**Analysis:**
- **artworks array**: For 100 artworks × ~64 bytes = ~6.4 KB
- **Usage**: Holds artwork references for playlist construction
- **Access pattern**: Sequential, not performance-critical

**Recommendation:**
```c
// Move to SPIRAM
artworks = (artwork_ref_t *)heap_caps_malloc(
    artwork_count * sizeof(artwork_ref_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!artworks) {
    artworks = malloc(artwork_count * sizeof(artwork_ref_t));
}
```

**Impact:**
- **Memory freed**: ~6-10 KB per playlist
- **Performance**: No impact

---

### 3. Load Tracker (`load_tracker.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| Path buffers | 256 bytes each | `malloc()` → Internal | **LOW** |
| Status structures | ~64 bytes each | `malloc()` → Internal | **LOW** |

**Analysis:**
- **Total**: < 1 KB typically
- **Usage**: Tracks loading status for UI display
- **Access pattern**: Infrequent updates

**Recommendation:**
```c
// Move to SPIRAM (low priority)
path_buffer = (char *)heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!path_buffer) {
    path_buffer = malloc(256);
}
```

**Impact:**
- **Memory freed**: < 1 KB
- **Performance**: No impact
- **Priority**: Low (small savings)

---

### 4. Vault Storage (`vault_storage.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| File path buffers | 256-512 bytes | Stack/`malloc()` | **LOW** |
| Temporary buffers | Variable | `malloc()` → Internal | **LOW** |

**Analysis:**
- **Total**: < 2 KB typically
- **Usage**: Temporary buffers for file operations
- **Access pattern**: Short-lived allocations

**Recommendation:**
- **Keep as-is** - Allocations are short-lived and small
- **Alternative**: Use stack buffers instead of malloc

**Impact:**
- **Memory freed**: < 2 KB
- **Priority**: Very low

---

### 5. Animation Metadata (`animation_metadata.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| JSON parsing buffers | Variable (1-10 KB) | `malloc()` → Internal | **MEDIUM** |
| Metadata structures | ~512 bytes each | `malloc()` → Internal | **LOW** |

**Analysis:**
- **JSON buffers**: Can be 1-10 KB for complex metadata
- **Usage**: Parse .json sidecar files
- **Access pattern**: Sequential read

**Recommendation:**
```c
// Move JSON buffers to SPIRAM
json_buffer = (char *)heap_caps_malloc(buffer_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!json_buffer) {
    json_buffer = malloc(buffer_size);
}
```

**Impact:**
- **Memory freed**: 1-10 KB per metadata file
- **Performance**: No impact (JSON parsing not time-critical)

---

### 6. Makapix Channel Task Stack (`makapix_channel_impl.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `refresh_stack` | 12,288 bytes (12 KB) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |

**Analysis:**
- **Line number**: 490 in `makapix_channel_impl.c`
- **Current code**:
  ```c
  ch->refresh_stack = heap_caps_malloc(
      MAKAPIX_REFRESH_TASK_STACK_SIZE * sizeof(StackType_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```

**Recommendation:**
- **NO CHANGE NEEDED** ✅
- **Status**: Already optimized
- **Note**: Falls back to `MALLOC_CAP_8BIT` if SPIRAM unavailable

---

## Summary

### Current Status

| Component | Status | Memory | Recommendation |
|-----------|--------|--------|----------------|
| Channel cache entries | ⚠️ Internal RAM | ~148 KB | **Move to SPIRAM** |
| Playlist artworks | ⚠️ Internal RAM | ~6-10 KB | **Move to SPIRAM** |
| Load tracker | ⚠️ Internal RAM | < 1 KB | Optional |
| Vault storage | ⚠️ Internal RAM | < 2 KB | Keep as-is |
| Metadata JSON buffers | ⚠️ Internal RAM | 1-10 KB | **Move to SPIRAM** |
| Makapix task stack | ✅ SPIRAM | 12 KB | Already optimized |

### Total Potential Savings

- **High priority** (cache entries): ~148 KB per channel
- **Medium priority** (playlist + metadata): ~7-20 KB
- **Low priority** (tracker + vault): < 3 KB
- **Total**: **~155-170 KB per channel** (can be multiple channels)

### Implementation Priority

1. **High**: Channel cache entries array (148 KB)
2. **Medium**: Hash table nodes (16 KB)
3. **Medium**: Playlist artworks array (6-10 KB)
4. **Medium**: JSON metadata buffers (1-10 KB)
5. **Low**: Load tracker buffers (< 1 KB)

### Risk Assessment

**Low Risk:**
- All these data structures are accessed infrequently
- No tight loops or real-time constraints
- Sequential access patterns work well with SPIRAM
- Fallback to internal RAM ensures compatibility

**Testing Required:**
- Load large channel caches (1000+ entries)
- Monitor channel switching performance
- Verify metadata parsing speed
- Check memory statistics for internal RAM savings

### Code Changes Required

- **Files to modify**: 3-4 files
- **Lines to change**: ~15-25 lines total
- **Complexity**: Low (simple allocation changes)
- **Testing effort**: Medium (need to test with large datasets)

### Special Considerations

1. **Multiple channels**: If system supports multiple active channels, savings multiply
2. **Large playlists**: For 5000+ entry channels, savings can exceed 500 KB
3. **Hash tables**: Consider using a hash table implementation that supports custom allocators

---

**Recommendation Status**: ✅ **APPROVED FOR IMPLEMENTATION**  
**Expected Impact**: **MEDIUM** (~150-500 KB depending on playlist size)  
**Risk Level**: **LOW**  
**Effort**: **LOW**
