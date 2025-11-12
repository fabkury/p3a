#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @brief Application state enumeration
 */
typedef enum {
    STATE_PLAYING = 0,    ///< Normal operation/idle state
    STATE_PROCESSING,     ///< Executing a command
    STATE_ERROR           ///< Unrecoverable error state
} app_state_t;

/**
 * @brief Initialize the application state module
 * 
 * Creates the mutex and sets initial state to PLAYING.
 * Must be called before any other app_state functions.
 */
void app_state_init(void);

/**
 * @brief Get the current application state
 * 
 * @return Current state (thread-safe)
 */
app_state_t app_state_get(void);

/**
 * @brief Get string representation of state
 * 
 * @param s State enum value
 * @return String representation ("PLAYING", "PROCESSING", "ERROR", or "UNKNOWN")
 */
const char* app_state_str(app_state_t s);

/**
 * @brief Transition to PLAYING state
 * 
 * Use this after successful command completion.
 */
void app_state_enter_playing(void);

/**
 * @brief Transition to PROCESSING state
 * 
 * Use this when starting command execution.
 */
void app_state_enter_processing(void);

/**
 * @brief Transition to ERROR state
 * 
 * Use this when an unrecoverable error occurs.
 * System will remain in ERROR state until reboot.
 */
void app_state_enter_error(void);

