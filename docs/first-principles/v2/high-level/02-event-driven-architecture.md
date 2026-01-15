# Event-Driven Architecture

> **Extends**: v1/high-level/central-event-bus.md  
> **Phase**: 2 (Core Architecture)

## Goal

Replace scattered callbacks and direct cross-module calls with a centralized event bus, making control flow explicit and components decoupled.

## Current State (v2 Assessment)

The codebase uses multiple communication patterns:

| Pattern | Example | Problem |
|---------|---------|---------|
| Direct callbacks | `http_api_set_action_handlers()` | Tight coupling |
| Registered callbacks | `p3a_state_register_callback()` | Multiple registration systems |
| Monitor tasks | `makapix_state_monitor_task()` | Polling instead of events |
| Weak symbol linkage | `__attribute__((weak))` handlers | Hidden dependencies |
| Global state polling | `makapix_get_state()` in loops | Inefficient, race-prone |

## v1 Alignment

v1 identifies the need for an event bus. v2 provides the event taxonomy and implementation details.

## Event Taxonomy

### System Events

```c
typedef enum {
    // Lifecycle
    EVENT_BOOT_COMPLETE,
    EVENT_SHUTDOWN_REQUESTED,
    
    // Connectivity (from wifi_manager, makapix)
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    EVENT_INTERNET_AVAILABLE,
    EVENT_INTERNET_LOST,
    EVENT_MQTT_CONNECTED,
    EVENT_MQTT_DISCONNECTED,
    EVENT_REGISTRATION_COMPLETE,
    EVENT_REGISTRATION_FAILED,
    
    // Storage
    EVENT_SD_MOUNTED,
    EVENT_SD_UNMOUNTED,
    EVENT_SD_ERROR,
} p3a_system_event_t;
```

### Content Events

```c
typedef enum {
    // Channel operations
    EVENT_CHANNEL_SWITCH_REQUESTED,
    EVENT_CHANNEL_LOADED,
    EVENT_CHANNEL_LOAD_FAILED,
    EVENT_CHANNEL_EMPTY,
    
    // Download
    EVENT_DOWNLOAD_STARTED,
    EVENT_DOWNLOAD_PROGRESS,    // payload: { percent, channel_id }
    EVENT_DOWNLOAD_COMPLETE,
    EVENT_DOWNLOAD_FAILED,
    
    // Playback
    EVENT_ARTWORK_AVAILABLE,    // New artwork ready to display
    EVENT_ARTWORK_DECODE_FAILED,
} p3a_content_event_t;
```

### Playback Events

```c
typedef enum {
    // Navigation
    EVENT_SWAP_NEXT,
    EVENT_SWAP_BACK,
    EVENT_SWAP_COMPLETE,
    
    // Timing
    EVENT_DWELL_TIMEOUT,        // Auto-swap timer fired
    EVENT_DWELL_RESET,          // User interaction reset timer
    
    // Mode changes
    EVENT_PICO8_STARTED,
    EVENT_PICO8_STOPPED,
    EVENT_PICO8_TIMEOUT,
} p3a_playback_event_t;
```

### UI Events

```c
typedef enum {
    // Provisioning flow
    EVENT_PROVISIONING_STARTED,
    EVENT_PROVISIONING_CODE_READY,
    EVENT_PROVISIONING_CANCELLED,
    EVENT_PROVISIONING_COMPLETE,
    
    // OTA flow
    EVENT_OTA_CHECK_STARTED,
    EVENT_OTA_UPDATE_AVAILABLE,
    EVENT_OTA_DOWNLOAD_PROGRESS,
    EVENT_OTA_INSTALL_STARTED,
    EVENT_OTA_INSTALL_COMPLETE,
    EVENT_OTA_FAILED,
    
    // Touch gestures
    EVENT_TOUCH_TAP,
    EVENT_TOUCH_SWIPE_UP,
    EVENT_TOUCH_SWIPE_DOWN,
    EVENT_TOUCH_LONG_PRESS,
    EVENT_TOUCH_ROTATE,
} p3a_ui_event_t;
```

## Event Bus Implementation

### Core API

```c
// Event structure
typedef struct {
    uint16_t type;              // Event type (from enums above)
    uint16_t category;          // System, Content, Playback, UI
    uint32_t timestamp;         // esp_timer_get_time() / 1000
    union {
        int32_t i32;
        uint32_t u32;
        void* ptr;              // Caller owns lifetime
        struct {
            int16_t x;
            int16_t y;
        } point;
    } payload;
} p3a_event_t;

// Subscription
typedef void (*p3a_event_handler_t)(const p3a_event_t* event, void* ctx);

esp_err_t event_bus_init(void);
esp_err_t event_bus_subscribe(uint16_t event_type, p3a_event_handler_t handler, void* ctx);
esp_err_t event_bus_subscribe_category(uint16_t category, p3a_event_handler_t handler, void* ctx);
void event_bus_unsubscribe(p3a_event_handler_t handler);

// Publishing
esp_err_t event_bus_emit(uint16_t event_type, const p3a_event_t* event);
esp_err_t event_bus_emit_simple(uint16_t event_type);  // No payload
esp_err_t event_bus_emit_i32(uint16_t event_type, int32_t value);
```

### Implementation Strategy

```c
// Internal structure
#define EVENT_QUEUE_SIZE 32
#define MAX_SUBSCRIBERS 48

typedef struct {
    uint16_t event_type;        // Specific event or 0xFFFF for category
    uint16_t category;          // Category filter (if event_type == 0xFFFF)
    p3a_event_handler_t handler;
    void* ctx;
} subscriber_t;

typedef struct {
    QueueHandle_t queue;
    subscriber_t subscribers[MAX_SUBSCRIBERS];
    size_t subscriber_count;
    TaskHandle_t dispatch_task;
    SemaphoreHandle_t mutex;
} event_bus_state_t;

static event_bus_state_t s_bus;
```

### Dispatch Task

```c
static void event_bus_dispatch_task(void* arg) {
    p3a_event_t event;
    while (true) {
        if (xQueueReceive(s_bus.queue, &event, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
            for (size_t i = 0; i < s_bus.subscriber_count; i++) {
                subscriber_t* sub = &s_bus.subscribers[i];
                bool match = (sub->event_type == event.type) ||
                             (sub->event_type == 0xFFFF && sub->category == event.category);
                if (match && sub->handler) {
                    sub->handler(&event, sub->ctx);
                }
            }
            xSemaphoreGive(s_bus.mutex);
        }
    }
}
```

## Migration Examples

### Before: Direct Callback

```c
// http_api.c
void http_api_set_action_handlers(action_callback_t swap_next, action_callback_t swap_back);

// Caller
http_api_set_action_handlers(app_lcd_cycle_animation, app_lcd_cycle_animation_backward);
```

### After: Event Emission

```c
// http_api.c - emits event
static esp_err_t handle_swap_next(httpd_req_t* req) {
    event_bus_emit_simple(EVENT_SWAP_NEXT);
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// play_scheduler.c - subscribes
void play_scheduler_init(void) {
    event_bus_subscribe(EVENT_SWAP_NEXT, on_swap_next, NULL);
    event_bus_subscribe(EVENT_SWAP_BACK, on_swap_back, NULL);
}
```

### Before: Monitor Task

```c
// p3a_main.c
static void makapix_state_monitor_task(void *arg) {
    while (true) {
        makapix_state_t state = makapix_get_state();
        if (state != last_state) {
            // Handle transition
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```

### After: Event-Driven

```c
// makapix.c - emits on state change
static void set_state(makapix_state_t new_state) {
    s_state = new_state;
    event_bus_emit_i32(EVENT_MAKAPIX_STATE_CHANGED, (int32_t)new_state);
}

// p3a_state.c - subscribes
void p3a_state_init(void) {
    event_bus_subscribe(EVENT_MAKAPIX_STATE_CHANGED, on_makapix_state, NULL);
}
```

## Debug Subscriber

```c
// For development: log all events
static void debug_event_logger(const p3a_event_t* event, void* ctx) {
    ESP_LOGI("EVENT", "type=%d cat=%d ts=%lu payload=%d",
             event->type, event->category, event->timestamp, event->payload.i32);
}

void enable_event_debugging(void) {
    // Subscribe to all categories
    event_bus_subscribe_category(CATEGORY_SYSTEM, debug_event_logger, NULL);
    event_bus_subscribe_category(CATEGORY_CONTENT, debug_event_logger, NULL);
    event_bus_subscribe_category(CATEGORY_PLAYBACK, debug_event_logger, NULL);
    event_bus_subscribe_category(CATEGORY_UI, debug_event_logger, NULL);
}
```

## Success Criteria

- [ ] No direct cross-module function calls for state changes
- [ ] Features can be added by emitting/consuming events
- [ ] Debug subscriber can log entire system behavior
- [ ] Monitor tasks eliminated in favor of event handlers
- [ ] `http_api_set_action_handlers()` and similar removed

## Risks

| Risk | Mitigation |
|------|------------|
| Event storms | Coalescing (latest-only for progress events) |
| Queue overflow | Backpressure: drop oldest, log warning |
| Handler exceptions | Catch and log; don't crash dispatch |
| Memory leaks | Events don't own payloads; clear semantics |
