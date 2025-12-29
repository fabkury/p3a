// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_touch_router.h
 * @brief State-aware touch event routing
 * 
 * Routes touch events to the appropriate handler based on current p3a state.
 * Each state has its own touch handling logic:
 * 
 * ANIMATION_PLAYBACK:
 * - Tap left/right: swap animation backward/forward
 * - Vertical swipe: brightness control
 * - Two-finger rotation: screen rotation
 * - Long press (4s): enter provisioning or toggle AP info
 * 
 * PROVISIONING:
 * - Long press (4s): cancel provisioning and return to playback
 * - Other gestures: ignored
 * 
 * OTA:
 * - All gestures: ignored (can't interrupt OTA)
 * 
 * PICO8_STREAMING:
 * - Touch events forwarded to USB HID
 * - Long press (4s): exit PICO-8 mode
 */

#ifndef P3A_TOUCH_ROUTER_H
#define P3A_TOUCH_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Touch event types
 */
typedef enum {
    P3A_TOUCH_EVENT_TAP_LEFT,       ///< Tap on left half of screen
    P3A_TOUCH_EVENT_TAP_RIGHT,      ///< Tap on right half of screen
    P3A_TOUCH_EVENT_SWIPE_UP,       ///< Vertical swipe upward
    P3A_TOUCH_EVENT_SWIPE_DOWN,     ///< Vertical swipe downward
    P3A_TOUCH_EVENT_BRIGHTNESS,     ///< Brightness adjustment (with delta)
    P3A_TOUCH_EVENT_LONG_PRESS,     ///< Long press (4 seconds)
    P3A_TOUCH_EVENT_ROTATION_CW,    ///< Two-finger clockwise rotation
    P3A_TOUCH_EVENT_ROTATION_CCW,   ///< Two-finger counter-clockwise rotation
    P3A_TOUCH_EVENT_RAW,            ///< Raw touch for PICO-8 forwarding
} p3a_touch_event_type_t;

/**
 * @brief Touch event data
 */
typedef struct {
    p3a_touch_event_type_t type;
    union {
        struct {
            int delta_percent;      ///< For BRIGHTNESS event
        } brightness;
        struct {
            uint16_t x;             ///< For RAW event
            uint16_t y;
            uint8_t pressure;
            bool pressed;           ///< true=down/move, false=up
        } raw;
    };
} p3a_touch_event_t;

/**
 * @brief Initialize touch router
 * 
 * Must be called after p3a_state_init().
 * 
 * @return ESP_OK on success
 */
esp_err_t p3a_touch_router_init(void);

/**
 * @brief Route a touch event to the appropriate handler
 * 
 * Called by the touch task when a gesture is recognized.
 * The event is routed based on current p3a state.
 * 
 * @param event Touch event to route
 * @return ESP_OK if handled, ESP_ERR_NOT_SUPPORTED if ignored
 */
esp_err_t p3a_touch_router_handle_event(const p3a_touch_event_t *event);

/**
 * @brief Check if gestures are enabled in current state
 * 
 * @param event_type Type of event to check
 * @return true if the gesture type is handled in current state
 */
bool p3a_touch_router_is_gesture_enabled(p3a_touch_event_type_t event_type);

#ifdef __cplusplus
}
#endif

#endif // P3A_TOUCH_ROUTER_H

