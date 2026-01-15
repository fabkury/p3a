# Task Consolidation

> **New in v2**  
> **Phase**: 4 (Cleanup and Optimization)

## Goal

Audit FreeRTOS tasks, consolidate where possible, and reduce overall task count to improve system determinism and reduce stack memory usage.

## Status

Completed (audit and IO merge deferred).

## Progress Checklist

- [ ] Audit current tasks and baseline stack usage
- [x] Replace polling tasks with event handlers
- [x] Convert dwell timing to FreeRTOS software timers
- [ ] Merge download/refresh into unified I/O task
- [x] Add lazy creation for upscale workers

## Final Decisions (Deferred Items)

- **Task audit + stack baseline**: Deferred because it needs runtime instrumentation or a build-time audit pass, which requires running on target hardware and is outside this code-only migration.
- **Merge download/refresh into unified I/O task**: Deferred because it would require a larger behavioral change across `download_manager` and `play_scheduler_refresh`, which risks regressions without a focused validation plan.

## Current State (v2 Assessment)

The codebase creates numerous FreeRTOS tasks:

| Task | Location | Purpose | Stack |
|------|----------|---------|-------|
| `animation_loader` | `animation_player.c` | Load assets from SD | 8KB+ |
| `memory_report` | `p3a_main.c` | Debug memory stats | 3KB |
| `refresh_task` | `play_scheduler_refresh.c` | Background refresh | 4KB |
| `download_task` | `download_manager.c` | Fetch from network | 8KB+ |
| `http_worker` | `http_api.c` | Command processing | 4KB |
| `pico8_timeout` | `pico8_stream.c` | Streaming timeout | 2KB |
| `upscale_worker_0` | `display_renderer.c` | Parallel upscale | 4KB |
| `upscale_worker_1` | `display_renderer.c` | Parallel upscale | 4KB |
| ESP-Hosted tasks | `esp_hosted` | WiFi communication | Multiple |
| TinyUSB tasks | `esp_tinyusb` | USB stack | Multiple |
| HTTP server | `esp_http_server` | Request handling | Multiple |
| MQTT client | `esp_mqtt` | Message handling | Multiple |

**Estimated**: 15+ application tasks, 50+ KB stack space.

## Problems with Current Task Usage

### 1. Polling Tasks

`makapix_state_monitor_task` polled every 500ms:

```c
while (true) {
    makapix_state_t current = makapix_get_state();
    if (current != last) { /* handle */ }
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

Replaced by event-driven handlers (task removed).

### 2. Single-Purpose Timer Tasks

`play_scheduler_timer.c` created a task just for dwell timeouts:

```c
static void timer_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (--remaining == 0) {
            play_scheduler_next(NULL);
        }
    }
}
```

Converted to a FreeRTOS software timer.

### 3. Parallel Workers Always Running

Upscale workers wait in a loop:

```c
while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // do work
    xSemaphoreGive(done_sem);
}
```

Good pattern, but consider if always needed.

## Consolidation Strategy

### Strategy 1: Replace Polling with Events

After event bus exists:

```c
// Before: polling task
while (true) {
    state = makapix_get_state();
    vTaskDelay(500);
}

// After: event handler (no task)
static void on_makapix_event(const p3a_event_t* ev, void* ctx) {
    // Handle state change immediately
}
event_bus_subscribe(EVENT_MAKAPIX_STATE_CHANGED, on_makapix_event, NULL);
```

**Eliminated tasks**: `makapix_state_monitor`

### Strategy 2: Use FreeRTOS Timers

```c
// Before: dedicated task
static void timer_task(void* arg) {
    while (true) {
        vTaskDelay(1000);
        check_dwell_timeout();
    }
}

// After: software timer (shared timer daemon)
static TimerHandle_t s_dwell_timer;

void init_dwell_timer(void) {
    s_dwell_timer = xTimerCreate("dwell", pdMS_TO_TICKS(1000), 
                                  pdTRUE, NULL, dwell_callback);
}
```

**Eliminated tasks**: `play_timer`

### Strategy 3: Consolidate Related Tasks

Merge tasks with similar purposes:

```c
// Before: separate tasks
- download_task (downloads from network)
- refresh_task (refreshes channel indexes)

// After: single background_io_task
static void background_io_task(void* arg) {
    while (true) {
        io_request_t req;
        if (xQueueReceive(s_io_queue, &req, portMAX_DELAY)) {
            switch (req.type) {
                case IO_DOWNLOAD: do_download(&req); break;
                case IO_REFRESH: do_refresh(&req); break;
            }
        }
    }
}
```

**Eliminated tasks**: Merge `download_task` + `refresh_task` → `background_io_task`

### Strategy 4: Lazy Worker Creation

Upscale workers only needed during playback:

```c
// Create on first use, delete when idle for 10s
void request_upscale_workers(void) {
    if (!s_workers_active) {
        create_upscale_workers();
        s_workers_active = true;
    }
    reset_idle_timer();
}

static void idle_timeout(TimerHandle_t timer) {
    if (s_workers_active && !rendering_active()) {
        delete_upscale_workers();
        s_workers_active = false;
    }
}
```

### Strategy 5: Remove Debug Tasks in Release

```c
#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
    xTaskCreate(memory_report_task, ...);
#endif
```

Already conditional, but ensure it's disabled in release builds.

## Proposed Task Map (After Consolidation)

| Task | Purpose | Notes |
|------|---------|-------|
| `animation_loader` | Asset loading | Keep (SD I/O blocking) |
| `background_io` | Downloads + refresh | Merged |
| `http_worker` | Command queue | Keep |
| `upscale_worker_*` | Parallel render | Lazy creation |
| ESP/USB/HTTP tasks | External stacks | Cannot change |

**Eliminated**:
- `makapix_state_monitor` → event handler
- `play_timer` → software timer
- `download_task` → merged into `background_io`
- `refresh_task` → merged into `background_io`
- `memory_report` → disabled in release

**Savings**: ~4 tasks, ~15 KB stack

## Implementation

### Step 1: Audit Current Tasks

```c
// Add to p3a_main.c during debug
void log_task_info(void) {
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t* tasks = malloc(count * sizeof(TaskStatus_t));
    uxTaskGetSystemState(tasks, count, NULL);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "Task: %s, stack: %lu", 
                 tasks[i].pcTaskName, tasks[i].usStackHighWaterMark);
    }
    free(tasks);
}
```

### Step 2: Convert Monitor Tasks to Events

After event bus (Phase 2), convert polling to subscriptions.

### Step 3: Merge IO Tasks

Create `background_io_task` with unified queue.

### Step 4: Add Lazy Worker Logic

Implement idle timeout for upscale workers.

## Success Criteria

- [ ] Application task count reduced by 30%
- [ ] No polling tasks (all event-driven)
- [ ] Stack usage documented per task
- [ ] Debug tasks disabled in release
- [ ] Task audit available via debug command

## Risks

| Risk | Mitigation |
|------|------------|
| Missed wake-up events | Event bus guarantees delivery |
| Queue starvation | Priority-based scheduling |
| Latency increase | Benchmark response times |
