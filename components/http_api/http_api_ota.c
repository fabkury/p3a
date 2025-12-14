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

// ---------- OTA Page Handler ----------

/**
 * GET /ota
 * Returns the OTA update web UI page
 */
static esp_err_t h_get_ota_page(httpd_req_t *req) {
    static const char ota_html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<title>p3a - Firmware Update</title>"
        "<style>"
        "* { box-sizing: border-box; }"
        "body {"
        "    margin: 0;"
        "    padding: 16px;"
        "    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    min-height: 100vh;"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "}"
        ".container { width: min(420px, 100%); }"
        "h1 {"
        "    text-align: center;"
        "    color: white;"
        "    font-size: 1.5rem;"
        "    font-weight: 300;"
        "    margin: 0 0 16px;"
        "    text-shadow: 0 2px 4px rgba(0,0,0,0.2);"
        "}"
        ".dev-badge {"
        "    display: inline-block;"
        "    background: #ff9800;"
        "    color: #000;"
        "    padding: 3px 8px;"
        "    border-radius: 4px;"
        "    font-size: 0.65rem;"
        "    font-weight: bold;"
        "    margin-left: 8px;"
        "    vertical-align: middle;"
        "}"
        ".card {"
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 16px;"
        "    padding: 16px;"
        "    margin-bottom: 12px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".version-row {"
        "    display: flex;"
        "    justify-content: space-between;"
        "    padding: 10px 0;"
        "    border-bottom: 1px solid #eee;"
        "}"
        ".version-row:last-child { border-bottom: none; }"
        ".version-label { color: #666; font-size: 0.9rem; }"
        ".version-value { color: #333; font-weight: 500; font-size: 0.9rem; }"
        ".update-available { color: #4CAF50; font-weight: 600; }"
        "button {"
        "    width: 100%;"
        "    padding: 14px;"
        "    border: none;"
        "    border-radius: 12px;"
        "    font-size: 0.95rem;"
        "    font-weight: 500;"
        "    cursor: pointer;"
        "    transition: all 0.2s;"
        "    margin-bottom: 10px;"
        "}"
        ".btn-primary {"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "    color: white;"
        "    box-shadow: 0 4px 12px rgba(102,126,234,0.4);"
        "}"
        ".btn-primary:active { transform: scale(0.98); }"
        ".btn-primary:disabled {"
        "    background: #ccc;"
        "    color: #888;"
        "    cursor: not-allowed;"
        "    box-shadow: none;"
        "}"
        ".btn-danger {"
        "    background: #ff6b6b;"
        "    color: white;"
        "    box-shadow: 0 4px 12px rgba(255,107,107,0.3);"
        "}"
        ".btn-danger:active { transform: scale(0.98); }"
        ".progress-container {"
        "    display: none;"
        "    background: rgba(255,255,255,0.95);"
        "    border-radius: 12px;"
        "    padding: 16px;"
        "    margin-bottom: 12px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "}"
        ".progress-bar {"
        "    height: 10px;"
        "    background: #e0e0e0;"
        "    border-radius: 5px;"
        "    overflow: hidden;"
        "}"
        ".progress-fill {"
        "    height: 100%;"
        "    background: linear-gradient(90deg, #667eea, #764ba2);"
        "    transition: width 0.3s;"
        "    width: 0%;"
        "}"
        ".progress-text {"
        "    text-align: center;"
        "    margin-top: 8px;"
        "    color: #666;"
        "    font-size: 0.85rem;"
        "}"
        ".status-message {"
        "    text-align: center;"
        "    padding: 12px;"
        "    border-radius: 8px;"
        "    margin-bottom: 12px;"
        "    display: none;"
        "    font-size: 0.9rem;"
        "}"
        ".status-success { background: #e8f5e9; color: #2e7d32; }"
        ".status-error { background: #ffebee; color: #c62828; }"
        ".status-info { background: #e3f2fd; color: #1565c0; }"
        ".release-notes {"
        "    max-height: 150px;"
        "    overflow-y: auto;"
        "    font-size: 0.8rem;"
        "    white-space: pre-wrap;"
        "    background: #f5f5f5;"
        "    padding: 12px;"
        "    border-radius: 8px;"
        "    margin-top: 12px;"
        "    display: none;"
        "    color: #333;"
        "}"
        ".back-link {"
        "    display: block;"
        "    text-align: center;"
        "    color: rgba(255,255,255,0.8);"
        "    text-decoration: none;"
        "    margin-top: 16px;"
        "    font-size: 0.9rem;"
        "}"
        ".back-link:active { color: white; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"container\">"
        "<h1>Firmware Update<span class=\"dev-badge\" id=\"dev-badge\" style=\"display:none\">DEV</span></h1>"
        "<div class=\"status-message\" id=\"status-msg\"></div>"
        "<div class=\"card\">"
        "<div class=\"version-row\"><span class=\"version-label\">Current Version</span><span class=\"version-value\" id=\"current-ver\">-</span></div>"
        "<div class=\"version-row\"><span class=\"version-label\">Available</span><span class=\"version-value\" id=\"available-ver\">-</span></div>"
        "<div class=\"version-row\"><span class=\"version-label\">Status</span><span class=\"version-value\" id=\"state\">-</span></div>"
        "</div>"
        "<div class=\"progress-container\" id=\"progress\">"
        "<div class=\"progress-bar\"><div class=\"progress-fill\" id=\"progress-fill\"></div></div>"
        "<div class=\"progress-text\" id=\"progress-text\">0%</div>"
        "</div>"
        "<button class=\"btn-primary\" id=\"check-btn\" onclick=\"checkUpdate()\">Check for Updates</button>"
        "<button class=\"btn-primary\" id=\"install-btn\" onclick=\"installUpdate()\" disabled>Install Update</button>"
        "<button class=\"btn-danger\" id=\"rollback-btn\" onclick=\"rollback()\" style=\"display:none\">Rollback to Previous</button>"
        "<div class=\"release-notes\" id=\"release-notes\"></div>"
        "<a href=\"/\" class=\"back-link\">&#8592; Back to Home</a>"
        "</div>"
        "<script>"
        "var pollInterval = null;"
        "var lastState = null;"
        "function showStatus(msg, type) {"
        "    var el = document.getElementById('status-msg');"
        "    el.textContent = msg;"
        "    el.className = 'status-message status-' + type;"
        "    el.style.display = 'block';"
        "    if (type !== 'info') setTimeout(function() { el.style.display = 'none'; }, 5000);"
        "}"
        "function hideStatus() {"
        "    document.getElementById('status-msg').style.display = 'none';"
        "}"
        "function updateUI(data) {"
        "    document.getElementById('current-ver').textContent = data.current_version || '-';"
        "    var availVerEl = document.getElementById('available-ver');"
        "    var verText = data.available_version || 'Up to date';"
        "    if (data.available_version && data.is_prerelease) verText += ' (pre-release)';"
        "    availVerEl.textContent = verText;"
        "    availVerEl.className = 'version-value' + (data.available_version ? ' update-available' : '');"
        "    var stateText = data.state.replace('_', ' ');"
        "    if (data.state === 'checking') stateText = 'Checking...';"
        "    document.getElementById('state').textContent = stateText;"
        "    document.getElementById('dev-badge').style.display = data.dev_mode ? 'inline-block' : 'none';"
        "    var checkBtn = document.getElementById('check-btn');"
        "    var installBtn = document.getElementById('install-btn');"
        "    var rollbackBtn = document.getElementById('rollback-btn');"
        "    var progressEl = document.getElementById('progress');"
        "    checkBtn.disabled = (data.state === 'checking' || data.state === 'downloading');"
        "    installBtn.disabled = (data.state !== 'update_available');"
        "    installBtn.style.display = (data.state === 'downloading' || data.state === 'verifying') ? 'none' : 'block';"
        "    rollbackBtn.style.display = data.can_rollback ? 'block' : 'none';"
        "    if (data.can_rollback) rollbackBtn.textContent = 'Rollback to ' + (data.rollback_version || 'Previous');"
        "    if (data.state === 'downloading' || data.state === 'verifying') {"
        "        progressEl.style.display = 'block';"
        "        var pct = data.download_progress || 0;"
        "        document.getElementById('progress-fill').style.width = pct + '%';"
        "        document.getElementById('progress-text').textContent = data.state === 'verifying' ? 'Verifying...' : pct + '%';"
        "    } else {"
        "        progressEl.style.display = 'none';"
        "    }"
        "    var notesEl = document.getElementById('release-notes');"
        "    if (data.release_notes && data.available_version) {"
        "        notesEl.textContent = data.release_notes;"
        "        notesEl.style.display = 'block';"
        "    } else {"
        "        notesEl.style.display = 'none';"
        "    }"
        "    if (lastState === 'checking' && data.state !== 'checking') {"
        "        hideStatus();"
        "        if (data.state === 'idle' && !data.available_version) {"
        "            showStatus('Firmware is up to date', 'success');"
        "        } else if (data.state === 'update_available') {"
        "            showStatus('Update available!', 'success');"
        "        }"
        "        if (pollInterval) { clearInterval(pollInterval); pollInterval = null; }"
        "    }"
        "    if (data.error_message) showStatus(data.error_message, 'error');"
        "    lastState = data.state;"
        "}"
        "function fetchStatus() {"
        "    fetch('/ota/status').then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) updateUI(d.data);"
        "    }).catch(function(e) { console.error('Status fetch error:', e); });"
        "}"
        "function checkUpdate() {"
        "    showStatus('Checking for updates...', 'info');"
        "    fetch('/ota/check', { method: 'POST' }).then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) {"
        "            startPolling();"
        "        } else {"
        "            showStatus(d.error || 'Check failed', 'error');"
        "        }"
        "    }).catch(function(e) { showStatus('Network error', 'error'); });"
        "}"
        "function installUpdate() {"
        "    if (!confirm('Install firmware update? The device will reboot.')) return;"
        "    showStatus('Starting update...', 'info');"
        "    fetch('/ota/install', { method: 'POST' }).then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) {"
        "            showStatus('Update in progress. Device will reboot...', 'info');"
        "            startPolling();"
        "        } else {"
        "            showStatus(d.error || 'Install failed', 'error');"
        "        }"
        "    }).catch(function(e) { showStatus('Network error', 'error'); });"
        "}"
        "function rollback() {"
        "    if (!confirm('Roll back to previous firmware? The device will reboot.')) return;"
        "    fetch('/ota/rollback', { method: 'POST' }).then(function(r) { return r.json(); }).then(function(d) {"
        "        if (d.ok) showStatus('Rolling back...', 'info');"
        "        else showStatus(d.error || 'Rollback failed', 'error');"
        "    }).catch(function(e) { showStatus('Network error', 'error'); });"
        "}"
        "function startPolling() {"
        "    if (pollInterval) clearInterval(pollInterval);"
        "    pollInterval = setInterval(fetchStatus, 1000);"
        "    setTimeout(function() { if (pollInterval) { clearInterval(pollInterval); pollInterval = null; } }, 120000);"
        "}"
        "fetchStatus();"
        "setInterval(fetchStatus, 5000);"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ota_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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

    return ESP_ERR_NOT_FOUND;
}

// ---------- Registration Function ----------

void http_api_register_ota_handlers(httpd_handle_t server) {
    // Kept for API stability; OTA endpoints are now served via the method routers (GET/POST /*).
    (void)server;
}

