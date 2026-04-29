// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_opc.h
 * @brief Optional Player Commands (OPC) for Makapix Club integration
 *
 * Implements the protocol described in inbox/p3a-optional-commands.md:
 *   - Publishes a retained `capabilities` manifest declaring which optional
 *     features the firmware supports (currently: pause, brightness, rotation).
 *   - Publishes a retained `state` topic with the current values for declared
 *     features. Republished whenever a value changes (server command, hardware
 *     control, schedule, fallback after error, ...).
 *   - Handles server-issued `set_paused`, `set_brightness`, `set_rotation`
 *     commands and acknowledges each one on the `command/ack` topic.
 *
 * The `mirror` feature is intentionally not advertised — p3a has no mirror
 * support. Incoming `set_mirror` commands are acked with `unsupported`.
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OPC module
 *
 * Snapshots current brightness and pause state, then subscribes to the event
 * bus events that signal value changes from any source (server command,
 * touch, REST, schedule, pause/resume side effects).
 *
 * Safe to call multiple times — re-initialization is a no-op.
 *
 * @return ESP_OK on success
 */
esp_err_t makapix_opc_init(void);

/**
 * @brief Publish the retained `capabilities` manifest
 *
 * Call once after MQTT connect, before publishing the first `state` message.
 *
 * @return ESP_OK on success
 */
esp_err_t makapix_opc_publish_capabilities(void);

/**
 * @brief Publish the full `state` (all declared fields) as a retained message
 *
 * Call once after MQTT connect (after `capabilities`). Subsequent state
 * publishes happen automatically as values change.
 *
 * @return ESP_OK on success
 */
esp_err_t makapix_opc_publish_state_full(void);

/**
 * @brief Try to handle an incoming MQTT command as an OPC command
 *
 * Recognized OPC command types: `set_paused`, `set_brightness`, `set_rotation`,
 * `set_mirror` (acked `unsupported`).
 *
 * Always sends a `command/ack` message for recognized OPC commands. The
 * subsequent `state` publish (per the spec) is handled automatically by the
 * event-bus subscribers when the underlying value actually changes.
 *
 * @param command_type Command type string from the MQTT envelope
 * @param payload Command payload object (may be NULL or empty)
 * @param command_id UUID from the envelope; required for ack
 * @return true if the command was an OPC command (and was acked); false to
 *         let the caller try other dispatchers.
 */
bool makapix_opc_handle_command(const char *command_type, const cJSON *payload,
                                const char *command_id);

#ifdef __cplusplus
}
#endif
