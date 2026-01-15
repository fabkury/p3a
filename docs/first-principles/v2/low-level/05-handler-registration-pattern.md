# Handler Registration Pattern

> **Extends**: v1/low-level/reduce-weak-symbol-coupling.md  
> **Phase**: 1 (Foundation)

## Goal

Replace `__attribute__((weak))` linkage with explicit handler registration, making dependencies discoverable and failure modes explicit.

## Current State (v2 Assessment)

### p3a_touch_router.c Uses Weak Symbols

```c
// Weak symbol declarations in p3a_touch_router.c
extern void animation_player_cycle_animation(bool forward) __attribute__((weak));
extern esp_err_t makapix_start_provisioning(void) __attribute__((weak));
extern void app_wifi_start_captive_portal(void) __attribute__((weak));
extern void app_lcd_enter_ui_mode(void) __attribute__((weak));
extern void app_lcd_exit_ui_mode(void) __attribute__((weak));
// ... more
```

**Problem**: If any handler is missing (not linked), calls silently become no-ops.

### play_scheduler.c Uses Weak Symbols

```c
extern esp_err_t animation_player_request_swap(const swap_request_t *request) 
    __attribute__((weak));
extern void animation_player_display_message(const char *title, const char *body) 
    __attribute__((weak));
```

**Problem**: Core functionality depends on optional linkage.

## v1 Alignment

v1's "Reduce Weak Symbol Coupling" identifies the problem and suggests registration. v2 provides the concrete pattern and migration path.

## Problems with Weak Symbols

### 1. Silent Failures

```c
if (animation_player_cycle_animation) {
    animation_player_cycle_animation(true);
}
// If not linked, this entire block is skipped silently
```

### 2. Hidden Dependencies

Dependencies are not visible in CMakeLists.txt or headers:

```cmake
# CMakeLists.txt doesn't show p3a_touch_router needs animation_player
idf_component_register(
    SRCS "p3a_touch_router.c"
    REQUIRES p3a_state  # animation_player not listed!
)
```

### 3. Difficult Testing

Cannot mock or stub handlers for unit testing.

## Handler Registration Pattern

### Define Handler Struct

```c
// p3a_handlers.h
#ifndef P3A_HANDLERS_H
#define P3A_HANDLERS_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Touch gesture handlers
 */
typedef struct {
    // Navigation
    void (*on_tap)(void);
    void (*on_swipe_up)(void);
    void (*on_swipe_down)(void);
    void (*on_swipe_left)(void);
    void (*on_swipe_right)(void);
    
    // Special gestures
    void (*on_long_press)(void);
    void (*on_rotate_cw)(void);
    void (*on_rotate_ccw)(void);
    
    // Mode control
    esp_err_t (*enter_ui_mode)(void);
    void (*exit_ui_mode)(void);
} p3a_touch_handlers_t;

/**
 * @brief Playback control handlers
 */
typedef struct {
    esp_err_t (*request_swap)(const void* request);
    void (*display_message)(const char* title, const char* body);
    bool (*is_animation_ready)(void);
} p3a_playback_handlers_t;

/**
 * @brief Connectivity handlers
 */
typedef struct {
    esp_err_t (*start_provisioning)(void);
    void (*cancel_provisioning)(void);
    void (*start_captive_portal)(void);
} p3a_connectivity_handlers_t;

#endif // P3A_HANDLERS_H
```

### Registration API

```c
// p3a_touch_router.h
#include "p3a_handlers.h"

/**
 * @brief Register touch gesture handlers
 * 
 * Must be called during initialization. Missing critical handlers
 * will be logged as warnings.
 * 
 * @param handlers Handler function pointers (can have NULL members)
 * @return ESP_OK on success
 */
esp_err_t p3a_touch_router_register_handlers(const p3a_touch_handlers_t* handlers);

/**
 * @brief Check if critical handlers are registered
 * 
 * @return true if all required handlers are present
 */
bool p3a_touch_router_handlers_ready(void);
```

### Implementation

```c
// p3a_touch_router.c
#include "p3a_touch_router.h"
#include "esp_log.h"

static const char* TAG = "touch_router";

static p3a_touch_handlers_t s_handlers = {0};
static bool s_handlers_registered = false;

esp_err_t p3a_touch_router_register_handlers(const p3a_touch_handlers_t* handlers)
{
    if (!handlers) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&s_handlers, handlers, sizeof(s_handlers));
    s_handlers_registered = true;
    
    // Log missing handlers
    if (!s_handlers.on_tap) {
        ESP_LOGW(TAG, "No tap handler registered");
    }
    if (!s_handlers.on_long_press) {
        ESP_LOGW(TAG, "No long press handler registered");
    }
    // ... check other critical handlers
    
    return ESP_OK;
}

bool p3a_touch_router_handlers_ready(void)
{
    return s_handlers_registered && 
           s_handlers.on_tap != NULL &&
           s_handlers.enter_ui_mode != NULL;
}

// Usage in gesture handling
static void handle_tap(void)
{
    if (s_handlers.on_tap) {
        s_handlers.on_tap();
    } else {
        ESP_LOGD(TAG, "Tap ignored (no handler)");
    }
}
```

### Registration in app_main

```c
// p3a_main.c
#include "p3a_touch_router.h"
#include "animation_player.h"
#include "makapix.h"
#include "app_wifi.h"

static void register_handlers(void)
{
    // Touch handlers
    p3a_touch_handlers_t touch = {
        .on_tap = play_scheduler_touch_next,
        .on_swipe_up = handle_brightness_up,
        .on_swipe_down = handle_brightness_down,
        .on_long_press = makapix_start_provisioning,
        .on_rotate_cw = handle_rotate_cw,
        .on_rotate_ccw = handle_rotate_ccw,
        .enter_ui_mode = app_lcd_enter_ui_mode,
        .exit_ui_mode = app_lcd_exit_ui_mode,
    };
    ESP_ERROR_CHECK(p3a_touch_router_register_handlers(&touch));
    
    // Playback handlers
    p3a_playback_handlers_t playback = {
        .request_swap = animation_player_request_swap,
        .display_message = animation_player_display_message,
        .is_animation_ready = animation_player_is_animation_ready,
    };
    ESP_ERROR_CHECK(play_scheduler_register_handlers(&playback));
    
    // Connectivity handlers
    p3a_connectivity_handlers_t conn = {
        .start_provisioning = makapix_start_provisioning,
        .cancel_provisioning = makapix_cancel_provisioning,
        .start_captive_portal = app_wifi_start_captive_portal,
    };
    ESP_ERROR_CHECK(p3a_touch_router_register_connectivity_handlers(&conn));
}

void app_main(void)
{
    // ... init code ...
    
    register_handlers();
    
    // Verify critical handlers
    if (!p3a_touch_router_handlers_ready()) {
        ESP_LOGE(TAG, "Critical handlers missing!");
        // Could enter error state
    }
    
    // ... rest of init ...
}
```

## Migration Steps

### Step 1: Create Handler Headers

Create `p3a_handlers.h` with struct definitions.

### Step 2: Add Registration Functions

Add `*_register_handlers()` to affected modules:
- `p3a_touch_router.c`
- `play_scheduler.c`
- Others using weak symbols

### Step 3: Implement Registration

Store handlers in static struct, add null checks.

### Step 4: Update app_main

Add explicit registration calls with all handlers.

### Step 5: Remove Weak Symbols

After registration works, remove `__attribute__((weak))` declarations.

### Step 6: Update CMakeLists

Make dependencies explicit:

```cmake
# p3a_core/CMakeLists.txt
idf_component_register(
    SRCS "p3a_touch_router.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_state
    # Note: handlers come from main via registration, not REQUIRES
)
```

## Testing Benefits

With registration, handlers can be mocked:

```c
// test_touch_router.c
static int tap_count = 0;
static void mock_on_tap(void) { tap_count++; }

void test_tap_handling(void)
{
    p3a_touch_handlers_t mock = {
        .on_tap = mock_on_tap,
    };
    p3a_touch_router_register_handlers(&mock);
    
    // Simulate tap
    simulate_touch_tap();
    
    TEST_ASSERT_EQUAL(1, tap_count);
}
```

## Success Criteria

- [ ] No `__attribute__((weak))` in production code
- [ ] All handler registrations in app_main or clear init sequence
- [ ] Missing handlers logged at boot
- [ ] Dependencies explicit in registration code
- [ ] Handlers can be mocked for testing

## Risks

| Risk | Mitigation |
|------|------------|
| Forgetting to register | Boot-time validation, clear errors |
| Order of initialization | Document init sequence requirements |
| More boilerplate | Single registration point, clear pattern |
