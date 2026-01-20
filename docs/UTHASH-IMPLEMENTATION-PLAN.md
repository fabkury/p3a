# uthash Integration Implementation Plan

**Document:** UTHASH-IMPLEMENTATION-PLAN.md  
**Status:** Design Document  
**Created:** 2026-01-20

---

## 1. Executive Summary

This document outlines the implementation plan for integrating uthash into the p3a codebase to provide O(1) hash table-based access to channel index (Ci) entries, replacing the current O(n) linear search implementations.

### Goals
- Replace O(n) linear search with O(1) hash table lookups in Ci
- Enable LAi to reference Ci entries by stable ID instead of array index
- Maintain backward compatibility with existing persisted data
- Minimize memory overhead while maximizing performance

---

## 2. Current Architecture Analysis

### 2.1 Channel Index (Ci) - Current Implementation

**Data Structure:**
```c
typedef struct {
    makapix_channel_entry_t *entries;   // Array of entries
    size_t entry_count;                 // Number of entries
    // ... other fields ...
} channel_cache_t;
```

**Entry Format:**
```c
typedef struct __attribute__((packed)) {
    int32_t post_id;                 // Makapix post_id
    uint8_t storage_key_uuid[16];    // UUID bytes
    // ... 64 bytes total ...
} makapix_channel_entry_t;
```

**Current Lookups (O(n)):**
- `ci_find_by_storage_key()`: Linear search through all entries
- `ci_find_by_post_id()`: Linear search through all entries
- Performance: ~512 comparisons average for 1024 entries

### 2.2 Locally Available Index (LAi) - Current Implementation

**Data Structure:**
```c
typedef struct {
    uint32_t *available_indices;  // Array of Ci array indices
    size_t available_count;       // Number of available entries
} channel_cache_t;
```

**Current Lookups (O(n)):**
- `lai_contains()`: Linear search through available_indices
- `lai_find_slot()`: Linear search through available_indices
- Performance: ~256 comparisons average for 512 available entries

**Problem with Array Indices:**
- LAi stores array positions, not stable IDs
- If Ci array is reordered or compacted, LAi indices become invalid
- No protection against array index changes

---

## 3. Design: Hash Table-Based Ci Access

### 3.1 Conceptual Approach

**Key Insight:** uthash allows adding hash table functionality without changing the on-disk format or memory layout of entries. The hash table is maintained in-memory only.

**Strategy:**
1. Keep existing `makapix_channel_entry_t` structure (64 bytes, packed)
2. Create wrapper structures that include uthash handles
3. Maintain multiple hash tables for different lookup keys
4. Build hash tables on load, update on modifications
5. LAi references entries by post_id (stable ID) instead of array index

### 3.2 Data Structures

#### Ci Hash Tables

We need two separate hash tables for Ci:
1. **By storage_key**: For download completion lookups
2. **By post_id**: For LAi references and general lookups

**Wrapper Structure:**
```c
// Hash table wrapper for Ci entries
typedef struct {
    makapix_channel_entry_t entry;  // Actual entry data (64 bytes)
    
    // uthash handles (in-memory only, not persisted)
    UT_hash_handle hh_by_storage_key;  // Hash by storage_key_uuid
    UT_hash_handle hh_by_post_id;      // Hash by post_id
} ci_entry_wrapper_t;
```

**Updated channel_cache_t:**
```c
typedef struct channel_cache_s {
    // Legacy array (for sequential access, persistence)
    makapix_channel_entry_t *entries;
    size_t entry_count;
    
    // NEW: Hash table heads
    ci_entry_wrapper_t *ht_by_storage_key;  // Hash table head
    ci_entry_wrapper_t *ht_by_post_id;      // Hash table head
    
    // LAi - now stores post_ids instead of array indices
    int32_t *available_post_ids;  // Changed from uint32_t indices
    size_t available_count;
    
    // LAi hash table for O(1) contains/find
    lai_entry_wrapper_t *lai_ht;  // Hash table for LAi
    
    // ... existing fields ...
} channel_cache_t;
```

#### LAi Hash Table

```c
// Hash table wrapper for LAi entries
typedef struct {
    int32_t post_id;           // Key (post_id from Ci)
    uint32_t lai_slot;         // Slot in available_post_ids array
    UT_hash_handle hh;         // Hash handle
} lai_entry_wrapper_t;
```

### 3.3 Memory Overhead Analysis

**Per Ci Entry:**
- Current: 64 bytes (entry only)
- With uthash: 64 + 2×(3×8) = 112 bytes per wrapper
- Overhead: 48 bytes per entry (75% increase)

**For 1024 entries:**
- Current: 65 KB
- With uthash: 115 KB
- Additional: 50 KB

**For LAi (512 available entries):**
- Current: 512 × 4 = 2 KB (indices)
- New: 512 × 4 = 2 KB (post_ids) + 512 × 16 = 8 KB (hash table)
- Additional: 8 KB

**Total per channel:** ~58 KB additional memory
**For 4 channels:** ~232 KB additional (acceptable for ESP32-P4 with 32MB PSRAM)

---

## 4. Implementation Details

### 4.1 Building Hash Tables on Load

```c
esp_err_t channel_cache_load(const char *channel_id,
                              const char *channels_path,
                              const char *vault_path,
                              channel_cache_t *cache)
{
    // 1. Load entries array from disk (existing code)
    // ... load entries ...
    
    // 2. Build hash tables
    cache->ht_by_storage_key = NULL;
    cache->ht_by_post_id = NULL;
    
    for (size_t i = 0; i < cache->entry_count; i++) {
        ci_entry_wrapper_t *wrapper = malloc(sizeof(ci_entry_wrapper_t));
        memcpy(&wrapper->entry, &cache->entries[i], sizeof(makapix_channel_entry_t));
        
        // Add to storage_key hash table
        HASH_ADD(hh_by_storage_key, cache->ht_by_storage_key, 
                 entry.storage_key_uuid, 16, wrapper);
        
        // Add to post_id hash table
        HASH_ADD(hh_by_post_id, cache->ht_by_post_id,
                 entry.post_id, sizeof(int32_t), wrapper);
    }
    
    // 3. Load LAi (now as post_ids) and build LAi hash table
    // ... load LAi ...
    lai_rebuild_hashtable(cache);
    
    return ESP_OK;
}
```

### 4.2 O(1) Lookup Functions

```c
uint32_t ci_find_by_storage_key(const channel_cache_t *cache, 
                                const uint8_t *storage_key_uuid)
{
    if (!cache || !storage_key_uuid) {
        return UINT32_MAX;
    }
    
    ci_entry_wrapper_t *wrapper;
    HASH_FIND(hh_by_storage_key, cache->ht_by_storage_key, 
              storage_key_uuid, 16, wrapper);
    
    if (!wrapper) {
        return UINT32_MAX;
    }
    
    // Return post_id (now used as stable ID instead of array index)
    return wrapper->entry.post_id;
}

const makapix_channel_entry_t *ci_get_by_post_id(const channel_cache_t *cache,
                                                  int32_t post_id)
{
    if (!cache) {
        return NULL;
    }
    
    ci_entry_wrapper_t *wrapper;
    HASH_FIND(hh_by_post_id, cache->ht_by_post_id,
              &post_id, sizeof(int32_t), wrapper);
    
    return wrapper ? &wrapper->entry : NULL;
}
```

### 4.3 LAi Operations with Hash Table

```c
bool lai_add_entry(channel_cache_t *cache, int32_t post_id)
{
    // Check if already in LAi hash table (O(1))
    lai_entry_wrapper_t *lai_entry;
    HASH_FIND_INT(cache->lai_ht, &post_id, lai_entry);
    if (lai_entry) {
        return false;  // Already present
    }
    
    // Add to available_post_ids array
    cache->available_post_ids[cache->available_count] = post_id;
    
    // Add to LAi hash table
    lai_entry = malloc(sizeof(lai_entry_wrapper_t));
    lai_entry->post_id = post_id;
    lai_entry->lai_slot = cache->available_count;
    HASH_ADD_INT(cache->lai_ht, post_id, lai_entry);
    
    cache->available_count++;
    cache->dirty = true;
    return true;
}

bool lai_contains(const channel_cache_t *cache, int32_t post_id)
{
    lai_entry_wrapper_t *lai_entry;
    HASH_FIND_INT(cache->lai_ht, &post_id, lai_entry);
    return lai_entry != NULL;
}

uint32_t lai_find_slot(const channel_cache_t *cache, int32_t post_id)
{
    lai_entry_wrapper_t *lai_entry;
    HASH_FIND_INT(cache->lai_ht, &post_id, lai_entry);
    return lai_entry ? lai_entry->lai_slot : UINT32_MAX;
}
```

### 4.4 Persistence Format

**No changes to on-disk format:**
- Ci entries: Still 64 bytes packed, array format
- LAi: Changed from `uint32_t` indices to `int32_t` post_ids
  - Old format: `[0, 5, 7, 12, ...]` (array indices)
  - New format: `[101, 105, 107, 112, ...]` (post_ids)

**Migration strategy:**
- Detect old format: LAi values are small (< entry_count)
- Detect new format: LAi values are large (post_id range)
- On load, if old format detected, convert indices to post_ids using the entries array

---

## 5. Files to Modify

### 5.1 Header Files

**`components/channel_manager/include/channel_cache.h`:**
- Add uthash include
- Add `ci_entry_wrapper_t` and `lai_entry_wrapper_t` structures
- Update `channel_cache_t` with hash table fields
- Update function signatures (change ci_index to post_id where appropriate)

**`components/channel_manager/include/makapix_channel_impl.h`:**
- No changes needed (entry structure stays 64 bytes packed)

### 5.2 Implementation Files

**`components/channel_manager/channel_cache.c`:**
- Add hash table initialization in `channel_cache_load()`
- Replace O(n) searches with HASH_FIND in lookup functions
- Update LAi operations to use hash tables
- Add migration logic for old LAi format
- Add hash table cleanup in `channel_cache_free()`

**`components/play_scheduler/play_scheduler_lai.c`:**
- Update to use post_ids instead of ci_indices
- Update LAi operations to work with post_ids

**`components/channel_manager/CMakeLists.txt`:**
- Add `uthash` to REQUIRES

### 5.3 Build System

**`components/channel_manager/CMakeLists.txt`:**
```cmake
idf_component_register(
    SRCS "..."
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES "uthash" "..."  # Add uthash
)
```

---

## 6. Expected Tradeoffs

### 6.1 Benefits

**Performance:**
- **Ci lookups:** O(n) → O(1)
  - `ci_find_by_storage_key()`: ~512 iterations → 1 hash lookup
  - `ci_find_by_post_id()`: ~512 iterations → 1 hash lookup
- **LAi operations:** O(n) → O(1)
  - `lai_contains()`: ~256 iterations → 1 hash lookup
  - `lai_find_slot()`: ~256 iterations → 1 hash lookup

**Typical use case impact:**
- Download completion: Saves ~512 comparisons per file
- Load failure handling: Saves ~256 comparisons per failure
- Web UI stats: Negligible (already returns LAi count directly)

**Scalability:**
- Linear time → Constant time as channel size grows
- 1024 entries: ~50x faster
- 4096 entries: ~200x faster

**Stability:**
- LAi now references by post_id (stable) instead of array index (fragile)
- Ci can be reordered, compacted, or modified without breaking LAi

### 6.2 Costs

**Memory:**
- ~58 KB per channel additional memory
- ~232 KB total for 4 channels
- Acceptable for ESP32-P4 (32 MB PSRAM)

**Complexity:**
- More complex code to maintain hash tables
- Need to keep hash tables synchronized with array
- Migration complexity for LAi format

**Build time:**
- Negligible (uthash is header-only)

**Maintenance:**
- Need to maintain hash table consistency on Ci modifications
- Hash tables must be rebuilt on cache load (one-time cost)

---

## 7. Migration Plan

### 7.1 Detection

```c
// Detect old LAi format (array indices)
bool is_old_lai_format(const channel_cache_t *cache)
{
    // Old format: values are small (< entry_count)
    // New format: values are post_ids (typically > 1000)
    
    if (cache->available_count == 0) {
        return false;  // Empty, doesn't matter
    }
    
    // Sample first entry
    int32_t first_val = cache->available_post_ids[0];
    
    // If value is within entry_count range, likely old format
    return (first_val >= 0 && first_val < (int32_t)cache->entry_count);
}
```

### 7.2 Conversion

```c
void migrate_lai_to_post_ids(channel_cache_t *cache)
{
    int32_t *new_lai = malloc(cache->available_count * sizeof(int32_t));
    
    for (size_t i = 0; i < cache->available_count; i++) {
        uint32_t old_index = (uint32_t)cache->available_post_ids[i];
        if (old_index < cache->entry_count) {
            // Convert array index to post_id
            new_lai[i] = cache->entries[old_index].post_id;
        }
    }
    
    memcpy(cache->available_post_ids, new_lai, 
           cache->available_count * sizeof(int32_t));
    free(new_lai);
    
    cache->dirty = true;  // Mark for save
    ESP_LOGI(TAG, "Migrated LAi from indices to post_ids");
}
```

---

## 8. Testing Plan

### 8.1 Unit Tests

- [ ] Test hash table creation from entries array
- [ ] Test `ci_find_by_storage_key()` with hash table
- [ ] Test `ci_find_by_post_id()` with hash table
- [ ] Test LAi hash table operations
- [ ] Test migration from old LAi format
- [ ] Test memory cleanup (no leaks)

### 8.2 Integration Tests

- [ ] Test cache load with existing data
- [ ] Test download completion with hash-based lookup
- [ ] Test load failure handling with hash-based LAi
- [ ] Test persistence of new LAi format
- [ ] Test multiple channels with hash tables

### 8.3 Performance Tests

- [ ] Benchmark `ci_find_by_storage_key()` before/after
- [ ] Benchmark `lai_contains()` before/after
- [ ] Measure memory usage increase
- [ ] Verify no performance regression in sequential access

---

## 9. Implementation Checklist

- [x] Add uthash component to codebase
- [ ] Update channel_cache.h with hash table structures
- [ ] Implement hash table building in cache_load()
- [ ] Replace ci_find functions with hash-based implementations
- [ ] Update LAi to use post_ids instead of indices
- [ ] Implement LAi hash table operations
- [ ] Add migration logic for old LAi format
- [ ] Update play scheduler to use post_ids
- [ ] Add memory cleanup for hash tables
- [ ] Update CMakeLists.txt dependencies
- [ ] Test and validate

---

## 10. Future Enhancements

### 10.1 Potential Optimizations

- Use `HASH_FIND_PTR` for pointer-based lookups
- Consider bloom filters for "not present" fast path
- Pool allocator for wrapper structures (reduce fragmentation)

### 10.2 Additional Hash Tables

If needed in the future:
- Hash by `created_at` for time-based queries
- Hash by `storage_key` string (if UUID conversion is expensive)

---

*This document serves as the authoritative design for uthash integration in p3a.*
