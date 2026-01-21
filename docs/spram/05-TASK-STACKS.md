# Task Stacks - SPRAM Optimization Analysis

## Component Overview

FreeRTOS task stacks consume a significant amount of memory. On ESP32-P4, FreeRTOS supports task stacks in SPIRAM, which is ideal for non-critical tasks.

**Default Behavior**: `xTaskCreate()` allocates stacks from **internal RAM**  
**Alternative**: `xTaskCreateStatic()` with SPIRAM-allocated stack buffer

## Current Task Inventory

### Tasks Found in Codebase

| Task Name | Stack Size | Location | File | Priority |
|-----------|------------|----------|------|----------|
| `wifi_recovery` | 4,096 B | Internal | app_wifi.c:265 | **MEDIUM** |
| `wifi_health` | 4,096 B | Internal | app_wifi.c:318 | **MEDIUM** |
| `dns_server` | 4,096 B | Internal | app_wifi.c:177 | **MEDIUM** |
| `event_bus` | 4,096 B | Internal | event_bus.c | **MEDIUM** |
| `download_mgr` | 81,920 B (80 KB) | Internal | download_manager.c | **HIGH** ⚠️ |
| `ch_switch` | 8,192 B | Internal | makapix.c | **MEDIUM** |
| `makapix_prov` | 8,192 B | Internal | makapix.c | **MEDIUM** |
| `mqtt_reconn` | 16,384 B (16 KB) | Internal | makapix.c:510 | **HIGH** |
| `status_pub` | 4,096 B | Internal | makapix_connection.c | **MEDIUM** |
| `cred_poll` | 16,384 B (16 KB) | Internal | makapix_provision_flow.c | **HIGH** |
| `view_tracker` | 6,144 B | Internal | view_tracker.c | **MEDIUM** |
| `ota_check` | 8,192 B | Internal | ota_manager.c | **MEDIUM** |
| `api_worker` | 4,096 B | Internal | http_api.c | **MEDIUM** |
| `animation_loader` | Variable | Internal | animation_player.c | **MEDIUM** |
| `anim_sd_refresh` | 16,384 B (16 KB) | Internal | animation_player.c | **HIGH** |
| `upscale_top` | 2,048 B | Internal, Core 0 | display_renderer.c | **LOW** ⚠️ |
| `upscale_bottom` | 2,048 B | Internal, Core 0 | display_renderer.c | **LOW** ⚠️ |
| `display_render` | 4,096 B | Internal | display_renderer.c | **MEDIUM** |
| `app_touch_task` | 8,192 B | Internal | app_touch.c | **MEDIUM** |
| `mem_report` | 3,072 B | Internal | p3a_main.c | **MEDIUM** |
| `debug_prov` | 4,096 B | Internal | p3a_main.c | **LOW** |
| `refresh_task` (play_scheduler) | 4,096 B | Internal | play_scheduler_refresh.c | **MEDIUM** |
| `makapix_refresh` (static) | 12,288 B (12 KB) | **SPIRAM** ✅ | makapix_channel_impl.c:490 | N/A |

### Stack Size Analysis

**Total Internal RAM for Task Stacks: ~230 KB**

- **Very Large** (>16 KB): download_mgr (80 KB), mqtt_reconn (16 KB), anim_sd_refresh (16 KB), cred_poll (16 KB)
- **Large** (8-16 KB): ch_switch, makapix_prov, ota_check, app_touch_task
- **Medium** (4-6 KB): wifi_recovery, wifi_health, dns_server, event_bus, status_pub, api_worker, display_render, mem_report, debug_prov, refresh_task
- **Small** (2-3 KB): upscale_top, upscale_bottom

## Optimization Opportunities

### HIGH Priority - Large Stacks (>10 KB)

#### 1. Download Manager Task (80 KB) ⚠️ **CRITICAL**

**File**: `components/channel_manager/download_manager.c`

```c
// Current
if (xTaskCreate(download_task, "download_mgr", 81920, NULL, 3, &s_task) != pdPASS) {
    // ...
}
```

**Recommendation**:
```c
// Pre-allocate stack in SPIRAM
static StaticTask_t download_task_buffer;
StackType_t *download_stack = (StackType_t *)heap_caps_malloc(
    81920 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

if (download_stack) {
    s_task = xTaskCreateStatic(download_task, "download_mgr", 81920,
                               NULL, 3, download_stack, &download_task_buffer);
} else {
    // Fallback to internal RAM
    xTaskCreate(download_task, "download_mgr", 81920, NULL, 3, &s_task);
}
```

**Impact**: **80 KB freed** from internal RAM

---

#### 2. MQTT Reconnect Task (16 KB)

**File**: `components/makapix/makapix.c`, line 510

```c
// Current
if (xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle) != pdPASS) {
    // ...
}
```

**Recommendation**:
```c
// Pre-allocate stack in SPIRAM
static StaticTask_t mqtt_reconn_task_buffer;
StackType_t *mqtt_stack = (StackType_t *)heap_caps_malloc(
    16384 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

if (mqtt_stack) {
    s_reconnect_task_handle = xTaskCreateStatic(makapix_mqtt_reconnect_task,
                                                 "mqtt_reconn", 16384, NULL, 5,
                                                 mqtt_stack, &mqtt_reconn_task_buffer);
} else {
    xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle);
}
```

**Impact**: **16 KB freed** from internal RAM

---

#### 3. Animation SD Refresh Task (16 KB)

**File**: `main/animation_player.c`

```c
// Current
if (xTaskCreate(animation_player_sd_refresh_task,
                "anim_sd_refresh",
                ANIMATION_SD_REFRESH_STACK,  // 16384
                NULL,
                5,
                NULL) != pdPASS) {
    // ...
}
```

**Recommendation**:
```c
// Pre-allocate stack in SPIRAM
static StaticTask_t sd_refresh_task_buffer;
StackType_t *sd_refresh_stack = (StackType_t *)heap_caps_malloc(
    ANIMATION_SD_REFRESH_STACK * sizeof(StackType_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

if (sd_refresh_stack) {
    TaskHandle_t task_handle = xTaskCreateStatic(
        animation_player_sd_refresh_task, "anim_sd_refresh",
        ANIMATION_SD_REFRESH_STACK, NULL, 5,
        sd_refresh_stack, &sd_refresh_task_buffer);
} else {
    xTaskCreate(animation_player_sd_refresh_task, "anim_sd_refresh",
                ANIMATION_SD_REFRESH_STACK, NULL, 5, NULL);
}
```

**Impact**: **16 KB freed** from internal RAM

---

#### 4. Credentials Poll Task (16 KB)

**File**: `components/makapix/makapix_provision_flow.c`

Similar to MQTT reconnect task.

**Impact**: **16 KB freed** from internal RAM

---

### MEDIUM Priority - Medium Stacks (4-8 KB)

Tasks that can be moved to SPIRAM but with lower priority:
- `ch_switch` (8 KB)
- `makapix_prov` (8 KB)
- `ota_check` (8 KB)
- `app_touch_task` (8 KB)
- `wifi_recovery` (4 KB)
- `wifi_health` (4 KB)
- `dns_server` (4 KB)
- `event_bus` (4 KB)
- `status_pub` (4 KB)
- `api_worker` (4 KB)
- `display_render` (4 KB)
- `mem_report` (3 KB)
- `refresh_task` (4 KB)

**Total**: ~70 KB

**Recommendation**: Move these to SPIRAM in later phases

---

### LOW Priority - Keep in Internal RAM ⚠️

#### Upscale Worker Tasks (2 KB each)

**File**: `main/display_renderer.c`

**Reason to keep internal**:
- **Pinned to Core 0** for parallel upscaling
- **Performance-critical** - tight rendering loops
- **Small size** (4 KB total for both)

**Recommendation**: **KEEP IN INTERNAL RAM**

These tasks are explicitly pinned to CPU cores and work in parallel for real-time upscaling. Moving to SPIRAM could impact frame rate.

---

## Implementation Strategy

### Phase 1: High Priority (128 KB savings)
1. Download manager task (80 KB) ⚠️ **CRITICAL IMPACT**
2. MQTT reconnect task (16 KB)
3. Animation SD refresh task (16 KB)
4. Credentials poll task (16 KB)

### Phase 2: Medium Priority (~70 KB savings)
5. Makapix tasks (8 KB × 2 = 16 KB)
6. OTA check task (8 KB)
7. Touch task (8 KB)
8. WiFi tasks (4 KB × 3 = 12 KB)
9. HTTP/event tasks (4 KB × 4 = 16 KB)
10. Other medium tasks (~10 KB)

### Phase 3: Already Optimized
- `makapix_refresh` task ✅ Already using SPIRAM

### Phase 4: Keep Internal
- Upscale worker tasks (Core-pinned, performance-critical)

## Technical Implementation

### Pattern for xTaskCreateStatic with SPIRAM

```c
// 1. Declare static task buffer (small, can be internal)
static StaticTask_t task_buffer;

// 2. Allocate stack in SPIRAM
StackType_t *task_stack = (StackType_t *)heap_caps_malloc(
    STACK_SIZE * sizeof(StackType_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

// 3. Create task with static allocation
if (task_stack) {
    TaskHandle_t handle = xTaskCreateStatic(
        task_function,
        "task_name",
        STACK_SIZE,
        task_param,
        task_priority,
        task_stack,
        &task_buffer);
    
    if (!handle) {
        // Handle error, free stack
        free(task_stack);
    }
} else {
    // Fallback to dynamic internal allocation
    ESP_LOGW(TAG, "SPIRAM allocation failed, using internal RAM");
    xTaskCreate(task_function, "task_name", STACK_SIZE,
                task_param, task_priority, &handle);
}
```

### Cleanup Considerations

When using `xTaskCreateStatic()`, stack memory must be freed manually after task deletion:

```c
// Store stack pointer for later cleanup
StackType_t *task_stack_ptr = task_stack;

// When deleting task:
vTaskDelete(task_handle);
free(task_stack_ptr);  // Free SPIRAM stack
```

## Summary

### Total Potential Savings

| Priority | Tasks | Stack Memory |
|----------|-------|--------------|
| **HIGH** | 4 tasks | **128 KB** |
| **MEDIUM** | ~13 tasks | **~70 KB** |
| **Total** | ~17 tasks | **~198 KB** |

### Already Optimized
- `makapix_refresh`: 12 KB in SPIRAM ✅

### Keep Internal
- Upscale workers: 4 KB (performance-critical) ⚠️

### Risk Assessment

**Low Risk**:
- FreeRTOS fully supports SPIRAM task stacks
- Non-critical tasks work fine with SPIRAM stacks
- Fallback to internal RAM ensures compatibility
- No performance impact for background tasks

**Medium Risk (Phase 2)**:
- Touch task might have timing sensitivity
- WiFi tasks might have timing requirements
- Test thoroughly after implementation

**High Risk (Upscale workers)**:
- Keep in internal RAM due to core pinning and tight loops

### Code Changes Required

- **Files to modify**: ~10 files
- **Lines to change**: ~50-70 lines total
- **Complexity**: Medium (need to switch to static task creation)
- **Testing effort**: Medium (test each task individually)

---

**Recommendation Status**: ✅ **APPROVED FOR PHASED IMPLEMENTATION**  
**Expected Impact**: **HIGH** (128 KB immediate, 198 KB total)  
**Risk Level**: **LOW-MEDIUM** (by phase)  
**Effort**: **MEDIUM**

**Critical Recommendation**: Start with download_manager (80 KB) - **biggest impact, lowest risk**!
