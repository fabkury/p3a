# Network Buffers - SPRAM Optimization Analysis

## Component Overview

The network subsystem includes HTTP server, MQTT client, OTA updates, and file downloads. These components use large buffers for network I/O that are ideal candidates for SPIRAM.

**Files:**
- `components/http_api/http_api.c`
- `components/http_api/http_api_rest.c`
- `components/http_api/http_api_upload.c`
- `components/makapix/makapix_mqtt.c`
- `components/makapix/makapix_artwork.c`
- `components/makapix/makapix_provision.c`
- `components/ota_manager/ota_manager.c`
- `components/ota_manager/github_ota.c`

## Current Allocations

### 1. GitHub OTA Buffers (`github_ota.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| GitHub API response | 131,072 bytes (128 KB) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |
| SHA256 response | 256 bytes | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |

**Analysis:**
- **Line numbers**: 84, 154 in `github_ota.c`
- **Current code**:
  ```c
  char *response_buffer = (char *)heap_caps_malloc(
      MAX_API_RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```

**Recommendation:**
- **NO CHANGE NEEDED** ✅
- **Status**: Already optimized with SPIRAM allocation and fallback

---

### 2. OTA Manager (`ota_manager.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| OTA download buffer | 4,096 bytes (4 KB) | `MALLOC_CAP_DMA` | **KEEP AS-IS** ✅ |

**Analysis:**
- **Line number**: 62 in `ota_manager.c`
- **Current code**:
  ```c
  uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  ```

**Recommendation:**
- **KEEP AS DMA** - OTA operations may require DMA-capable memory
- **Note**: On ESP32-P4, PSRAM IS DMA-capable, but keeping explicit DMA flag is safer
- **Status**: Correctly allocated

---

### 3. MQTT Reassembly Buffer (`makapix_mqtt.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `s_reassembly_buffer` | 131,072 bytes (128 KB) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |
| Owned message copy | Variable (up to 128 KB) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |

**Analysis:**
- **Line numbers**: ~920, ~940 in `makapix_mqtt.c`
- **Current code**:
  ```c
  s_reassembly_buffer = (char *)heap_caps_malloc(
      MQTT_REASSEMBLY_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  
  char *owned = (char *)heap_caps_malloc(
      s_reassembly_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```

**Recommendation:**
- **NO CHANGE NEEDED** ✅
- **Status**: Already optimized with SPIRAM allocation

---

### 4. Makapix Artwork Download (`makapix_artwork.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| Chunk buffer | 131,072 bytes (128 KB) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |

**Analysis:**
- **Line number**: ~80 in `makapix_artwork.c`
- **Current code**:
  ```c
  uint8_t *chunk_buffer = heap_caps_malloc(DOWNLOAD_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
  ```

**Recommendation:**
- **NO CHANGE NEEDED** ✅
- **Status**: Already using SPIRAM
- **Note**: Missing `MALLOC_CAP_8BIT` fallback flag (minor issue)

**Suggested enhancement**:
```c
uint8_t *chunk_buffer = heap_caps_malloc(DOWNLOAD_CHUNK_SIZE,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

---

### 5. Makapix Provisioning (`makapix_provision.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| Credentials buffer | 16,384 bytes (16 KB) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |
| Setup buffer | 2,048 bytes (2 KB) | `malloc()` → Internal | **MEDIUM** |

**Analysis:**
- **Line numbers**: ~140 (credentials), ~50 (setup) in `makapix_provision.c`
- **Current code**:
  ```c
  .buffer = heap_caps_malloc(CREDENTIALS_MAX_RESPONSE_SIZE,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
  
  // Setup buffer (needs verification)
  setup_buffer = malloc(MAX_RESPONSE_SIZE);
  ```

**Recommendation:**
```c
// Move setup buffer to SPIRAM
setup_buffer = (char *)heap_caps_malloc(MAX_RESPONSE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!setup_buffer) {
    setup_buffer = malloc(MAX_RESPONSE_SIZE);
}
```

**Impact:**
- **Memory freed**: 2 KB
- **Priority**: Medium (small but easy win)

---

### 6. HTTP API Buffers (`http_api.c`, `http_api_rest.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| JSON config buffer | 32,768 bytes (32 KB) | `malloc()` → Internal | **MEDIUM** |
| WebSocket frame buffer | 8,246 bytes (~8 KB) | Stack | **LOW** |
| File upload buffers | Variable | Streaming | N/A |

**Analysis:**
- **JSON buffer**: Used for `/config` and `/status` endpoints
- **WebSocket**: Stack-allocated in `http_api_pico8.c` (WS_MAX_FRAME_SIZE)
- **Location**: `http_api_internal.h` defines MAX_JSON = 32 KB

**Recommendation:**
```c
// Move JSON buffer to SPIRAM (allocated per request)
json_buffer = (char *)heap_caps_malloc(MAX_JSON,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!json_buffer) {
    json_buffer = malloc(MAX_JSON);
}

// WebSocket frame buffer: Consider moving to heap if stack space is tight
// Current stack usage: 8246 bytes per WebSocket handler
```

**Impact:**
- **Memory freed**: 32 KB per HTTP request (temporary allocation)
- **WebSocket**: 8 KB if moved from stack to heap+SPIRAM
- **Priority**: Medium

**Note**: Need to verify exact allocation locations in http_api_rest.c

---

### 7. Loader Service (`loader_service.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| File buffer | Variable (file size) | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |

**Analysis:**
- **Line number**: 40 in `loader_service.c`
- **Current code**:
  ```c
  uint8_t *buffer = (uint8_t *)heap_caps_malloc(
      (size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
      buffer = (uint8_t *)malloc((size_t)file_size);
  }
  ```

**Recommendation:**
- **NO CHANGE NEEDED** ✅
- **Status**: Already optimized with proper fallback

---

## Summary

### Current Status

| Component | Status | Memory | Recommendation |
|-----------|--------|--------|----------------|
| GitHub OTA response | ✅ SPIRAM | 128 KB | Already optimized |
| OTA download buffer | ✅ DMA | 4 KB | Keep as-is |
| MQTT reassembly | ✅ SPIRAM | 128 KB | Already optimized |
| Artwork download | ✅ SPIRAM | 128 KB | Already optimized |
| Provisioning credentials | ✅ SPIRAM | 16 KB | Already optimized |
| Provisioning setup | ⚠️ Internal | 2 KB | Move to SPIRAM |
| HTTP JSON buffer | ⚠️ Internal | 32 KB | Move to SPIRAM |
| WebSocket frame | ⚠️ Stack | 8 KB | Optional heap+SPIRAM |
| Loader file buffer | ✅ SPIRAM | Variable | Already optimized |

### Total Potential Savings

- **Already optimized**: 128 + 128 + 128 + 16 = 400 KB ✅
- **Can be optimized**: 2 + 32 = 34 KB
- **Optional** (WebSocket): 8 KB

**Total new savings**: **34-42 KB**

### Implementation Priority

1. **Medium**: HTTP JSON config buffer (32 KB)
2. **Medium**: Provisioning setup buffer (2 KB)
3. **Low**: WebSocket frame buffer (8 KB) - only if stack space is limited

### Risk Assessment

**Very Low Risk:**
- Network buffers are accessed sequentially
- No real-time constraints for HTTP/MQTT message processing
- SPIRAM speed is adequate for network I/O (network is slower)
- All allocations already have or can have fallback to internal RAM

**Testing Required:**
- HTTP API `/config` and `/status` endpoints
- MQTT message reception (already tested)
- Provisioning flow
- Monitor response times (should be unchanged)

### Code Changes Required

- **Files to modify**: 2-3 files
- **Lines to change**: ~5-10 lines total
- **Complexity**: Very low (simple allocation changes)
- **Testing effort**: Low (existing code paths, just different allocator)

### Observations

**Good News**: Most network buffers are **already using SPIRAM** ✅

The codebase shows excellent memory management for network components:
- All large buffers (128 KB) already in SPIRAM
- Proper fallback patterns in place
- DMA buffers correctly identified

Only small improvements needed for remaining internal allocations.

---

**Recommendation Status**: ✅ **APPROVED FOR IMPLEMENTATION**  
**Expected Impact**: **LOW** (34-42 KB - most already optimized)  
**Risk Level**: **VERY LOW**  
**Effort**: **VERY LOW**

**Note**: This component is a **success story** - demonstrating that the codebase already follows best practices for large network buffers!
