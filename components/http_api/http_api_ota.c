// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_ota.c
 * @brief HTTP API OTA handlers - Firmware update functionality
 * 
 * Contains handlers for:
 * - GET /ota (OTA update page)
 * - GET /ota/status (OTA status)
 * - POST /ota/check (trigger update check)
 * - POST /ota/install (start firmware installation)
 * - POST /ota/rollback (rollback to previous firmware)
 */

#include "http_api_internal.h"
#include "ota_manager.h"
#include "animation_player.h"
#include "ugfx_ui.h"
#include "freertos/task.h"

// ---------- OTA Callbacks ----------

/**
 * OTA UI callback - controls animation player and LCD during OTA
 */
static void ota_ui_callback(bool enter, const char *version_from, const char *version_to) {
    if (enter) {
        animation_player_enter_ui_mode();
        ugfx_ui_show_ota_progress(version_from, version_to);
    } else {
        ugfx_ui_hide_ota_progress();
        animation_player_exit_ui_mode();
    }
}

/**
 * OTA progress callback - updates LCD progress display
 */
static void ota_progress_callback(int percent, const char *status_text) {
    ugfx_ui_update_ota_progress(percent, status_text);
}

// ---------- OTA REST Handlers ----------

/**
 * GET /ota/status
 * Returns current OTA status including version info and update availability
 */
static esp_err_t h_get_ota_status(httpd_req_t *req) {
    ota_status_t status;
    esp_err_t err = ota_manager_get_status(&status);
    if (err != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to get OTA status\",\"code\":\"OTA_STATUS_FAIL\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        if (root) cJSON_Delete(root);
        if (data) cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    
    cJSON_AddStringToObject(data, "state", ota_state_to_string(status.state));
    cJSON_AddStringToObject(data, "current_version", status.current_version);
    
    if (strlen(status.available_version) > 0) {
        cJSON_AddStringToObject(data, "available_version", status.available_version);
        cJSON_AddNumberToObject(data, "available_size", status.available_size);
        if (strlen(status.release_notes) > 0) {
            cJSON_AddStringToObject(data, "release_notes", status.release_notes);
        }
    } else {
        cJSON_AddNullToObject(data, "available_version");
        cJSON_AddNullToObject(data, "available_size");
    }
    
    if (status.last_check_time > 0) {
        cJSON_AddNumberToObject(data, "last_check", (double)status.last_check_time);
    } else {
        cJSON_AddNullToObject(data, "last_check");
    }
    
    if (status.state == OTA_STATE_DOWNLOADING) {
        cJSON_AddNumberToObject(data, "download_progress", status.download_progress);
    } else {
        cJSON_AddNullToObject(data, "download_progress");
    }
    
    if (status.state == OTA_STATE_ERROR && strlen(status.error_message) > 0) {
        cJSON_AddStringToObject(data, "error_message", status.error_message);
    } else {
        cJSON_AddNullToObject(data, "error_message");
    }
    
    cJSON_AddBoolToObject(data, "can_rollback", status.can_rollback);
    if (status.can_rollback && strlen(status.rollback_version) > 0) {
        cJSON_AddStringToObject(data, "rollback_version", status.rollback_version);
    } else {
        cJSON_AddNullToObject(data, "rollback_version");
    }
    
    // Dev mode info
    cJSON_AddBoolToObject(data, "dev_mode", status.dev_mode);
    cJSON_AddBoolToObject(data, "is_prerelease", status.is_prerelease);
    
    cJSON_AddItemToObject(root, "data", data);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    
    send_json(req, 200, json_str);
    free(json_str);
    return ESP_OK;
}

/**
 * POST /ota/check
 * Triggers an immediate update check
 */
static esp_err_t h_post_ota_check(httpd_req_t *req) {
    esp_err_t err = ota_manager_check_for_update();
    
    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Check already in progress\",\"code\":\"CHECK_IN_PROGRESS\"}");
        return ESP_OK;
    }
    
    if (err != ESP_OK) {
        char response[128];
        snprintf(response, sizeof(response), 
                 "{\"ok\":false,\"error\":\"Failed to start check: %s\",\"code\":\"CHECK_FAIL\"}", 
                 esp_err_to_name(err));
        send_json(req, 500, response);
        return ESP_OK;
    }
    
    send_json(req, 202, "{\"ok\":true,\"data\":{\"checking\":true,\"message\":\"Update check started\"}}");
    return ESP_OK;
}

/**
 * POST /ota/install
 * Starts firmware installation (device will reboot on success)
 */
static esp_err_t h_post_ota_install(httpd_req_t *req) {
    // Check if update is available
    ota_state_t state = ota_manager_get_state();
    if (state != OTA_STATE_UPDATE_AVAILABLE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"No update available\",\"code\":\"NO_UPDATE\"}");
        return ESP_OK;
    }
    
    // Check blockers
    const char *block_reason;
    if (ota_manager_is_blocked(&block_reason)) {
        char response[256];
        snprintf(response, sizeof(response), 
                 "{\"ok\":false,\"error\":\"%s\",\"code\":\"OTA_BLOCKED\"}", 
                 block_reason);
        send_json(req, 423, response);
        return ESP_OK;
    }
    
    // Send response before starting (we'll reboot after)
    send_json(req, 202, "{\"ok\":true,\"data\":{\"installing\":true,\"message\":\"Firmware update started. Device will reboot when complete.\"}}");
    
    // Small delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Start installation (this will reboot on success)
    // Pass callbacks for progress and UI control
    esp_err_t err = ota_manager_install_update(ota_progress_callback, ota_ui_callback);
    
    // If we get here, installation failed
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "OTA install failed: %s", esp_err_to_name(err));
        // Can't send response - connection likely closed
    }
    
    return ESP_OK;
}

/**
 * POST /ota/rollback
 * Schedules rollback to previous firmware and reboots
 */
static esp_err_t h_post_ota_rollback(httpd_req_t *req) {
    ota_status_t status;
    ota_manager_get_status(&status);
    
    if (!status.can_rollback) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"No rollback available\",\"code\":\"NO_ROLLBACK\"}");
        return ESP_OK;
    }
    
    // Send response before rebooting
    char response[256];
    snprintf(response, sizeof(response), 
             "{\"ok\":true,\"data\":{\"rolling_back\":true,\"target_version\":\"%s\",\"message\":\"Rolling back. Device will reboot.\"}}",
             status.rollback_version);
    send_json(req, 202, response);
    
    // Small delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Perform rollback (this will reboot)
    esp_err_t err = ota_manager_rollback();
    
    // If we get here, rollback failed
    if (err != ESP_OK) {
        ESP_LOGE(HTTP_API_TAG, "Rollback failed: %s", esp_err_to_name(err));
    }
    
    return ESP_OK;
}

// ---------- Web UI OTA Handlers ----------

/**
 * GET /ota/webui/status
 * Returns current web UI OTA status
 */
static esp_err_t h_get_webui_ota_status(httpd_req_t *req) {
    webui_ota_status_t status;
    esp_err_t err = webui_ota_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        if (root) cJSON_Delete(root);
        if (data) cJSON_Delete(data);
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);

    if (err == ESP_OK) {
        cJSON_AddStringToObject(data, "current_version",
                                strlen(status.current_version) > 0 ? status.current_version : "unknown");

        if (status.update_available && strlen(status.available_version) > 0) {
            cJSON_AddStringToObject(data, "available_version", status.available_version);
        } else {
            cJSON_AddNullToObject(data, "available_version");
        }

        cJSON_AddBoolToObject(data, "update_available", status.update_available);
        cJSON_AddBoolToObject(data, "partition_valid", status.partition_valid);
        cJSON_AddBoolToObject(data, "needs_recovery", status.needs_recovery);
        cJSON_AddBoolToObject(data, "auto_update_disabled", status.auto_update_disabled);
        cJSON_AddNumberToObject(data, "failure_count", status.failure_count);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        cJSON_AddStringToObject(data, "message", "Web UI OTA is disabled");
    }

    cJSON_AddItemToObject(root, "data", data);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"OOM\",\"code\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, 200, json_str);
    free(json_str);
    return ESP_OK;
}

/**
 * POST /ota/webui/repair
 * Triggers a forced re-download of the web UI
 */
static esp_err_t h_post_webui_ota_repair(httpd_req_t *req) {
    esp_err_t err = webui_ota_trigger_repair();

    if (err == ESP_ERR_NOT_SUPPORTED) {
        send_json(req, 501, "{\"ok\":false,\"error\":\"Web UI OTA is disabled\",\"code\":\"NOT_SUPPORTED\"}");
        return ESP_OK;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        send_json(req, 409, "{\"ok\":false,\"error\":\"Repair already in progress\",\"code\":\"REPAIR_IN_PROGRESS\"}");
        return ESP_OK;
    }

    if (err != ESP_OK) {
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"error\":\"Failed to start repair: %s\",\"code\":\"REPAIR_FAIL\"}",
                 esp_err_to_name(err));
        send_json(req, 500, response);
        return ESP_OK;
    }

    send_json(req, 202, "{\"ok\":true,\"data\":{\"repairing\":true,\"message\":\"Web UI repair started\"}}");
    return ESP_OK;
}

// ---------- OTA Page Handler ----------

/**
 * GET /ota
 * Serves the OTA update page from LittleFS
 */
static esp_err_t h_get_ota_page(httpd_req_t *req) {
    return serve_file(req, "/spiffs/ota.html");
}

// ---------- Sub-router entrypoints ----------

esp_err_t http_api_ota_route_get(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(uri, "/ota") == 0) {
        return h_get_ota_page(req);
    }
    if (strcmp(uri, "/ota/status") == 0) {
        return h_get_ota_status(req);
    }
    if (strcmp(uri, "/ota/webui/status") == 0) {
        return h_get_webui_ota_status(req);
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t http_api_ota_route_post(httpd_req_t *req) {
    const char *uri = req ? req->uri : NULL;
    if (!uri) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(uri, "/ota/check") == 0) {
        return h_post_ota_check(req);
    }
    if (strcmp(uri, "/ota/install") == 0) {
        return h_post_ota_install(req);
    }
    if (strcmp(uri, "/ota/rollback") == 0) {
        return h_post_ota_rollback(req);
    }
    if (strcmp(uri, "/ota/webui/repair") == 0) {
        return h_post_webui_ota_repair(req);
    }

    return ESP_ERR_NOT_FOUND;
}

// ---------- Registration Function ----------

void http_api_register_ota_handlers(httpd_handle_t server) {
    // Kept for API stability; OTA endpoints are now served via the method routers (GET/POST /*).
    (void)server;
}

