// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "ugfx_ui.h"
#include "esp_log.h"
#include "app_lcd.h"
#include "app_wifi.h"
#include "bsp/display.h"
#include "makapix_mqtt.h"
#include "makapix.h"
#include "config_store.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "version.h"
#include "esp_timer.h"
#include "p3a_state.h"
#include "esp_wifi_remote.h"
#include "giphy.h"
#include "storage_eviction.h"
#include "esp_heap_caps.h"
#include "play_scheduler.h"

// Include µGFX headers
#include "gfx.h"
#include "gdisp/gdisp.h"

static const char *TAG = "ugfx_ui";

// UI mode enumeration
typedef enum {
    UI_MODE_NONE,              // No UI active
    UI_MODE_STATUS,            // Provisioning status
    UI_MODE_REGISTRATION,      // Registration code display
    UI_MODE_CAPTIVE_AP_INFO,   // Captive portal setup info
    UI_MODE_OTA_PROGRESS,      // OTA update progress
    UI_MODE_CHANNEL_MESSAGE,   // Channel loading/download status
    UI_MODE_CONNECTIVITY_ERROR, // Connectivity error (no internet, etc.)
    UI_MODE_INFO_SCREEN         // System info overlay
} ui_mode_t;

// UI state
static bool s_ui_active = false;
static bool s_ugfx_initialized = false;
static time_t s_expires_time = 0;
static char s_current_code[16] = {0};
static char s_status_message[128] = {0};
static ui_mode_t s_ui_mode = UI_MODE_NONE;
static gOrientation s_pending_orientation = gOrientation0;  // Orientation to apply when µGFX inits

// OTA progress state
static int s_ota_progress = 0;
static char s_ota_status_text[64] = {0};
static char s_ota_version_from[32] = {0};
static char s_ota_version_to[32] = {0};

// Channel message state
static char s_channel_name[64] = {0};
static char s_channel_message[128] = {0};
static int s_channel_progress = -1;

// External variables for board_framebuffer.h (used by µGFX driver)
void *ugfx_framebuffer_ptr = NULL;
int ugfx_screen_width = 0;
int ugfx_screen_height = 0;
size_t ugfx_line_stride = 0;

// Forward declaration of our custom µGFX extension
extern void gdisp_lld_set_framebuffer(void *pixels, gCoord linelen);

/**
 * @brief Initialize µGFX or update its framebuffer pointer
 * 
 * On first call, initializes µGFX. On subsequent calls, updates the
 * internal framebuffer pointer so µGFX draws to the correct buffer.
 */
static esp_err_t ugfx_ui_init_gfx(uint8_t *framebuffer, size_t stride)
{
    if (!framebuffer) {
        ESP_LOGE(TAG, "Framebuffer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Set framebuffer for µGFX driver (used during gfxInit)
    ugfx_framebuffer_ptr = framebuffer;
    ugfx_screen_width = EXAMPLE_LCD_H_RES;
    ugfx_screen_height = EXAMPLE_LCD_V_RES;
    ugfx_line_stride = stride;

    if (s_ugfx_initialized) {
        // Already initialized - update µGFX's internal framebuffer pointer directly
        gdisp_lld_set_framebuffer(framebuffer, (gCoord)stride);
        
        // Ensure orientation is applied (may have changed since last frame)
        gOrientation current_orientation = gdispGetOrientation();
        if (current_orientation != s_pending_orientation) {
            gdispSetOrientation(s_pending_orientation);
            ESP_LOGD(TAG, "Applied orientation change: %d -> %d", current_orientation, s_pending_orientation);
        }
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Initializing µGFX with framebuffer %p, dimensions %dx%d, stride=%zu", 
             framebuffer, ugfx_screen_width, ugfx_screen_height, stride);

    gfxInit();
    s_ugfx_initialized = true;
    
    // Apply any pending orientation that was set before initialization
    if (s_pending_orientation != gOrientation0) {
        gdispSetOrientation(s_pending_orientation);
        ESP_LOGD(TAG, "Applied pending orientation: %d", s_pending_orientation);
    }
    
    ESP_LOGD(TAG, "µGFX initialized: display size %dx%d", gdispGetWidth(), gdispGetHeight());
    return ESP_OK;
}

/**
 * @brief Draw the captive portal AP info screen
 */
static void ugfx_ui_draw_captive_ap_info(void)
{
    gdispClear(GFX_BLACK);

    // Title
    gdispFillStringBox(0, 60, gdispGetWidth(), 36, "WiFi Setup Instructions",
                     gdispOpenFont("* DejaVu Sans 24"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Instructions (multi-line, smaller font)
    int y_pos = 120;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 36, "1. Connect to the WiFi network:",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    y_pos += 45;
    // Using CONFIG_ESP_AP_SSID directly (EXAMPLE_ESP_AP_SSID wrapper is local to app_wifi.c)
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 36, CONFIG_ESP_AP_SSID,
                     gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0x00FF00), GFX_BLACK, gJustifyCenter);

    y_pos += 50;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 36, "2. Open your web browser",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    y_pos += 45;
    {
        char hn[24];
        char url_buf[48];
        config_store_get_hostname(hn, sizeof(hn));
        snprintf(url_buf, sizeof(url_buf), "3. Go to: http://%s.local", hn);
        gdispFillStringBox(0, y_pos, gdispGetWidth(), 36, url_buf,
                         gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    }

    y_pos += 45;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 36, "or http://192.168.4.1",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    y_pos += 50;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 36, "4. Enter your WiFi credentials",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    // Dismiss hint
    gdispFillStringBox(0, gdispGetHeight() - 60, gdispGetWidth(), 25, "Long-press to dismiss",
                     gdispOpenFont("* DejaVu Sans 16"), HTML2COLOR(0x888888), GFX_BLACK, gJustifyCenter);
}

/**
 * @brief Draw the connectivity error screen (e.g. no internet)
 */
static void ugfx_ui_draw_connectivity_error(void)
{
    gdispClear(GFX_BLACK);

    gCoord screen_w = gdispGetWidth();
    gCoord screen_h = gdispGetHeight();

    // Title
    gdispFillStringBox(0, screen_h / 2 - 120, screen_w, 36, "No Internet Connection",
                     gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0xFF6666), GFX_BLACK, gJustifyCenter);

    // Description
    gdispFillStringBox(0, screen_h / 2 - 50, screen_w, 36, "Wi-Fi connected but no internet access",
                     gdispOpenFont("* DejaVu Sans 20"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Hint
    gdispFillStringBox(0, screen_h / 2 + 10, screen_w, 36, "Check your router or network settings",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    // Dismiss hint
    gdispFillStringBox(0, screen_h - 60, screen_w, 25, "Long-press to dismiss",
                     gdispOpenFont("* DejaVu Sans 16"), HTML2COLOR(0x888888), GFX_BLACK, gJustifyCenter);
}

/**
 * @brief Draw the provisioning status screen
 */
static void ugfx_ui_draw_status(void)
{
    gdispClear(GFX_BLACK);

    // Title
    gdispFillStringBox(0, 80, gdispGetWidth(), 30, "PROVISIONING",
                     gdispOpenFont("* DejaVu Sans 24"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Status message (large, centered)
    gdispFillStringBox(0, gdispGetHeight()/2 - 40, gdispGetWidth(), 50, s_status_message,
                     gdispOpenFont("* DejaVu Sans 32"), HTML2COLOR(0xFFFF00), GFX_BLACK, gJustifyCenter);

    // Sub-text
    gdispFillStringBox(0, gdispGetHeight()/2 + 40, gdispGetWidth(), 50, "Please wait...",
                     gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
}

/**
 * @brief Draw the OTA progress screen
 */
static void ugfx_ui_draw_ota_progress(void)
{
    gdispClear(GFX_BLACK);
    
    gCoord screen_w = gdispGetWidth();
    gCoord screen_h = gdispGetHeight();

    // Title
    gdispFillStringBox(0, 60, screen_w, 35, "FIRMWARE UPDATE",
                     gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0x00FF88), GFX_BLACK, gJustifyCenter);

    // Version info
    char version_text[96];
    if (strlen(s_ota_version_from) > 0 && strlen(s_ota_version_to) > 0) {
        snprintf(version_text, sizeof(version_text), "v%s  ->  v%s", s_ota_version_from, s_ota_version_to);
    } else {
        snprintf(version_text, sizeof(version_text), "Installing update...");
    }
    gdispFillStringBox(0, 110, screen_w, 30, version_text,
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    // Progress bar background
    gCoord bar_x = 40;
    gCoord bar_y = screen_h / 2 - 20;
    gCoord bar_w = screen_w - 80;
    gCoord bar_h = 40;
    
    // Draw bar outline
    gdispDrawBox(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, HTML2COLOR(0x444444));
    
    // Draw bar background
    gdispFillArea(bar_x, bar_y, bar_w, bar_h, HTML2COLOR(0x222222));
    
    // Draw progress fill
    gCoord fill_w = (bar_w * s_ota_progress) / 100;
    if (fill_w > 0) {
        // Gradient-like effect using two shades
        gdispFillArea(bar_x, bar_y, fill_w, bar_h / 2, HTML2COLOR(0x00FF88));
        gdispFillArea(bar_x, bar_y + bar_h / 2, fill_w, bar_h / 2, HTML2COLOR(0x00CC6A));
    }

    // Progress percentage
    char progress_text[16];
    snprintf(progress_text, sizeof(progress_text), "%d%%", s_ota_progress);
    gdispFillStringBox(0, bar_y + bar_h + 20, screen_w, 40, progress_text,
                     gdispOpenFont("* DejaVu Sans 32"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Status text
    gdispFillStringBox(0, bar_y + bar_h + 80, screen_w, 30, s_ota_status_text,
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xFFFF00), GFX_BLACK, gJustifyCenter);

    // Warning at bottom
    gdispFillStringBox(0, screen_h - 60, screen_w, 25, "DO NOT POWER OFF",
                     gdispOpenFont("* DejaVu Sans 16"), HTML2COLOR(0xFF6666), GFX_BLACK, gJustifyCenter);
}

/**
 * @brief Draw the channel loading/downloading message screen
 */
static void ugfx_ui_draw_channel_message(void)
{
    gdispClear(GFX_BLACK);
    
    gCoord screen_w = gdispGetWidth();
    gCoord screen_h = gdispGetHeight();

    // Channel name at top
    if (strlen(s_channel_name) > 0) {
        gdispFillStringBox(0, 60, screen_w, 35, s_channel_name,
                         gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0x00AAFF), GFX_BLACK, gJustifyCenter);
    }

    // Main status message (supports '\n' line breaks)
    gFont msg_font = gdispOpenFont("* DejaVu Sans 24");
    const char *msg = s_channel_message;
    if (!msg) msg = "";

    // Split into up to 3 lines on '\n' (rendering '\n' directly shows as '?' on some builds)
    enum { MAX_LINES = 3 };
    const char *line_start[MAX_LINES] = {0};
    size_t line_len[MAX_LINES] = {0};
    int line_count = 0;

    const char *p = msg;
    line_start[line_count] = p;
    while (*p && line_count < MAX_LINES) {
        if (*p == '\r') {
            // ignore CR (Windows newlines)
            p++;
            continue;
        }
        if (*p == '\n') {
            line_len[line_count] = (size_t)(p - line_start[line_count]);
            line_count++;
            if (line_count >= MAX_LINES) break;
            p++; // skip '\n'
            line_start[line_count] = p;
            continue;
        }
        p++;
    }
    if (line_count < MAX_LINES) {
        line_len[line_count] = (size_t)(p - line_start[line_count]);
        line_count++;
    }
    if (line_count <= 0) line_count = 1;

    // Compute line height and vertical placement (centered around middle)
    gCoord line_h = gdispGetFontMetric(msg_font, gFontLineSpacing);
    if (line_h <= 0) line_h = 28;
    gCoord block_h = (gCoord)(line_count * line_h);
    gCoord start_y = (screen_h / 2) - (block_h / 2);

    for (int i = 0; i < line_count; i++) {
        char line_buf[128];
        size_t n = line_len[i];
        if (n >= sizeof(line_buf)) n = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start[i], n);
        line_buf[n] = '\0';

        gdispFillStringBox(0, start_y + (gCoord)(i * line_h), screen_w, line_h, line_buf,
                           msg_font, GFX_WHITE, GFX_BLACK, gJustifyCenter);
    }

    // Progress bar (if progress is defined, i.e. >= 0)
    if (s_channel_progress >= 0) {
        gCoord bar_x = 60;
        gCoord bar_y = screen_h / 2 + 30;
        gCoord bar_w = screen_w - 120;
        gCoord bar_h = 24;
        
        // Draw bar outline
        gdispDrawBox(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, HTML2COLOR(0x444444));
        
        // Draw bar background
        gdispFillArea(bar_x, bar_y, bar_w, bar_h, HTML2COLOR(0x222222));
        
        // Draw progress fill
        int progress = s_channel_progress;
        if (progress > 100) progress = 100;
        gCoord fill_w = (bar_w * progress) / 100;
        if (fill_w > 0) {
            gdispFillArea(bar_x, bar_y, fill_w, bar_h, HTML2COLOR(0x00AAFF));
        }

        // Progress percentage
        char progress_text[16];
        snprintf(progress_text, sizeof(progress_text), "%d%%", progress);
        gdispFillStringBox(0, bar_y + bar_h + 15, screen_w, 30, progress_text,
                         gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    }

    // Hint at bottom
    gdispFillStringBox(0, screen_h - 60, screen_w, 25, "Please wait...",
                     gdispOpenFont("* DejaVu Sans 16"), HTML2COLOR(0x888888), GFX_BLACK, gJustifyCenter);
}

/**
 * @brief Map Makapix state to display string
 */
static const char *makapix_state_to_string(makapix_state_t state)
{
    switch (state) {
        case MAKAPIX_STATE_IDLE:                  return "Idle";
        case MAKAPIX_STATE_PROVISIONING:          return "Provisioning";
        case MAKAPIX_STATE_SHOW_CODE:             return "Show Code";
        case MAKAPIX_STATE_CONNECTING:            return "Connecting";
        case MAKAPIX_STATE_CONNECTED:             return "Connected";
        case MAKAPIX_STATE_DISCONNECTED:          return "Disconnected";
        case MAKAPIX_STATE_REGISTRATION_INVALID:  return "Invalid Registration";
        default:                                  return "Unknown";
    }
}

/**
 * @brief Draw the system info screen
 */
static void ugfx_ui_draw_info_screen(void)
{
    gdispClear(GFX_BLACK);

    gCoord screen_w = gdispGetWidth();
    gFont font_title = gdispOpenFont("* DejaVu Sans 24");
    gFont font_main  = gdispOpenFont("* DejaVu Sans 20");
    gFont font_hint  = gdispOpenFont("* DejaVu Sans 16");

    color_t gray   = HTML2COLOR(0x888888);
    color_t green  = HTML2COLOR(0x00FF00);
    color_t yellow = HTML2COLOR(0xFFFF00);
    color_t red    = HTML2COLOR(0xFF6666);

    gCoord lx = 40;       // label x
    gCoord vx = 280;      // value x
    gCoord vw = screen_w - vx - 20;  // value width
    gCoord lw = vx - lx - 10;        // label width

    // Title – show device name (e.g. "p3a" or "p3a-kitchen")
    char title_buf[64];
    if (config_store_get_hostname(title_buf, sizeof(title_buf)) != ESP_OK) {
        snprintf(title_buf, sizeof(title_buf), "p3a");
    }
    gdispFillStringBox(0, 50, screen_w, 35, title_buf,
                       font_title, GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // --- Row 1: Firmware ---
    gCoord y = 120;
    gCoord rh = 36;   // row height – enough for descenders (p, y, g…)
    gdispFillStringBox(lx, y, lw, rh, "Firmware", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        char fw_buf[48];
        snprintf(fw_buf, sizeof(fw_buf), "v%s", FW_VERSION);
        gdispFillStringBox(vx, y, vw, rh, fw_buf, font_main, GFX_WHITE, GFX_BLACK, gJustifyLeft);
    }

    // --- Row 2: Uptime ---
    y = 170;
    gdispFillStringBox(lx, y, lw, rh, "Uptime", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        int64_t us = esp_timer_get_time();
        int total_sec = (int)(us / 1000000);
        int days  = total_sec / 86400;
        int hours = (total_sec % 86400) / 3600;
        int mins  = (total_sec % 3600) / 60;
        char uptime_buf[32];
        if (days > 0) {
            snprintf(uptime_buf, sizeof(uptime_buf), "%dd %dh %dm", days, hours, mins);
        } else if (hours > 0) {
            snprintf(uptime_buf, sizeof(uptime_buf), "%dh %dm", hours, mins);
        } else {
            snprintf(uptime_buf, sizeof(uptime_buf), "%dm", mins);
        }
        gdispFillStringBox(vx, y, vw, rh, uptime_buf, font_main, GFX_WHITE, GFX_BLACK, gJustifyLeft);
    }

    // --- Row 3: Connectivity ---
    y = 220;
    gdispFillStringBox(lx, y, lw, rh, "Connectivity", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        p3a_connectivity_level_t conn = p3a_state_get_connectivity();
        const char *conn_msg = p3a_state_get_connectivity_message();
        color_t conn_color;
        switch (conn) {
            case P3A_CONNECTIVITY_ONLINE:           conn_color = green;  break;
            case P3A_CONNECTIVITY_NO_MQTT:
            case P3A_CONNECTIVITY_NO_REGISTRATION:  conn_color = yellow; break;
            default:                                conn_color = red;    break;
        }
        gdispFillStringBox(vx, y, vw, rh, conn_msg ? conn_msg : "Unknown",
                           font_main, conn_color, GFX_BLACK, gJustifyLeft);
    }

    // --- Row 4: WiFi Signal ---
    y = 270;
    gdispFillStringBox(lx, y, lw, rh, "WiFi Signal", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_remote_sta_get_ap_info(&ap) == ESP_OK) {
            const char *quality;
            color_t rssi_color;
            if (ap.rssi >= -50) {
                quality = "Excellent"; rssi_color = green;
            } else if (ap.rssi >= -60) {
                quality = "Good";      rssi_color = green;
            } else if (ap.rssi >= -70) {
                quality = "Fair";      rssi_color = yellow;
            } else {
                quality = "Weak";      rssi_color = red;
            }
            char rssi_buf[40];
            snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm (%s)", ap.rssi, quality);
            gdispFillStringBox(vx, y, vw, rh, rssi_buf, font_main, rssi_color, GFX_BLACK, gJustifyLeft);
        } else {
            gdispFillStringBox(vx, y, vw, rh, "N/A", font_main, gray, GFX_BLACK, gJustifyLeft);
        }
    }

    // --- Row 5: Makapix ---
    y = 320;
    gdispFillStringBox(lx, y, lw, rh, "Makapix", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        makapix_state_t mstate = makapix_get_state();
        const char *mstr = makapix_state_to_string(mstate);
        color_t mcolor;
        switch (mstate) {
            case MAKAPIX_STATE_CONNECTED:            mcolor = green;  break;
            case MAKAPIX_STATE_CONNECTING:
            case MAKAPIX_STATE_DISCONNECTED:
            case MAKAPIX_STATE_SHOW_CODE:
            case MAKAPIX_STATE_PROVISIONING:         mcolor = yellow; break;
            case MAKAPIX_STATE_REGISTRATION_INVALID:
            case MAKAPIX_STATE_IDLE:                 mcolor = red;    break;
            default:                                 mcolor = gray;   break;
        }
        gdispFillStringBox(vx, y, vw, rh, mstr, font_main, mcolor, GFX_BLACK, gJustifyLeft);
    }

    // --- Row 6: Giphy ---
    y = 370;
    gdispFillStringBox(lx, y, lw, rh, "Giphy", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        giphy_refresh_status_t gs = giphy_get_last_refresh_status();
        const char *gs_str;
        color_t gs_color;
        switch (gs) {
            case GIPHY_REFRESH_OK:            gs_str = "OK";      gs_color = green; break;
            case GIPHY_REFRESH_FAILED:        gs_str = "Failed";  gs_color = red;   break;
            default:                          gs_str = "N/A";     gs_color = gray;  break;
        }
        gdispFillStringBox(vx, y, vw, rh, gs_str, font_main, gs_color, GFX_BLACK, gJustifyLeft);
    }

    // --- Row 7: SD Card ---
    y = 420;
    gdispFillStringBox(lx, y, lw, rh, "SD Card", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        uint64_t total_bytes = 0, free_bytes = 0;
        if (storage_eviction_get_storage_info(&total_bytes, &free_bytes) == ESP_OK && total_bytes > 0) {
            int free_pct = (int)((free_bytes * 100) / total_bytes);
            float free_gb = (float)free_bytes / (1024.0f * 1024.0f * 1024.0f);
            float total_gb = (float)total_bytes / (1024.0f * 1024.0f * 1024.0f);
            char sd_buf[48];
            snprintf(sd_buf, sizeof(sd_buf), "%d%% free (%.1f / %.1f GB)", free_pct, free_gb, total_gb);
            color_t sd_color = (free_pct > 20) ? green : (free_pct > 5) ? yellow : red;
            gdispFillStringBox(vx, y, vw, rh, sd_buf, font_main, sd_color, GFX_BLACK, gJustifyLeft);
        } else {
            gdispFillStringBox(vx, y, vw, rh, "Not mounted", font_main, red, GFX_BLACK, gJustifyLeft);
        }
    }

    // --- Row 8: Free Heap ---
    y = 470;
    gdispFillStringBox(lx, y, lw, rh, "Free Heap", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        float free_kb = (float)free_heap / 1024.0f;
        char heap_buf[32];
        snprintf(heap_buf, sizeof(heap_buf), "%.0f KB free", free_kb);
        color_t heap_color = (free_kb > 100) ? green : (free_kb > 30) ? yellow : red;
        gdispFillStringBox(vx, y, vw, rh, heap_buf, font_main, heap_color, GFX_BLACK, gJustifyLeft);
    }

    // --- Row 9: Web UI ---
    y = 520;
    gdispFillStringBox(lx, y, lw, rh, "Web UI", font_main, gray, GFX_BLACK, gJustifyLeft);
    {
        char url_buf[80];
        snprintf(url_buf, sizeof(url_buf), "http://%s.local/", title_buf);
        gdispFillStringBox(vx, y, vw, rh, url_buf, font_main, GFX_WHITE, GFX_BLACK, gJustifyLeft);
    }

    // Dismiss hint at bottom
    gdispFillStringBox(0, gdispGetHeight() - 60, screen_w, 30, "Long-press to dismiss",
                       font_hint, HTML2COLOR(0x555555), GFX_BLACK, gJustifyCenter);
}

/**
 * @brief Draw the UI layout to the current framebuffer
 */
static void ugfx_ui_draw_layout(int32_t remaining_secs)
{
    gdispClear(GFX_BLACK);

    // Title
    gdispFillStringBox(0, 50, gdispGetWidth(), 35, "REGISTER PLAYER",
                     gdispOpenFont("* DejaVu Sans 24"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Registration code (large, green)
    gdispFillStringBox(0, gdispGetHeight()/2 - 100, gdispGetWidth(), 60, s_current_code,
                     gdispOpenFont("* DejaVu Sans 32"), HTML2COLOR(0x00FF00), GFX_BLACK, gJustifyCenter);

    // Instructions
    gdispFillStringBox(0, gdispGetHeight()/2 - 10, gdispGetWidth(), 35, "Enter this code at:",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    gdispFillStringBox(0, gdispGetHeight()/2 + 35, gdispGetWidth(), 35, "https://makapix.club/",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0x00BFFF), GFX_BLACK, gJustifyCenter);

    // Countdown timer (prominent, below instructions)
    // Note: Expiration is handled in ugfx_ui_render_to_buffer() which auto-exits provisioning
    // Ensure remaining_secs is positive for correct modulo calculation
    if (remaining_secs < 0) {
        remaining_secs = 0;
    }
    int minutes = remaining_secs / 60;
    int seconds = remaining_secs % 60;
    char timer_text[32];
    snprintf(timer_text, sizeof(timer_text), "Expires in %02d:%02d", minutes, seconds);
    // Color changes as time runs out: green > yellow > red
    color_t timer_color;
    if (remaining_secs > 300) {  // > 5 minutes: green
        timer_color = HTML2COLOR(0x00FF00);
    } else if (remaining_secs > 60) {  // > 1 minute: yellow
        timer_color = HTML2COLOR(0xFFFF00);
    } else {  // < 1 minute: red
        timer_color = HTML2COLOR(0xFF4444);
    }
    gdispFillStringBox(0, gdispGetHeight()/2 + 90, gdispGetWidth(), 45, timer_text,
                     gdispOpenFont("* DejaVu Sans 24"), timer_color, GFX_BLACK, gJustifyCenter);

    // Bottom status area
    int bottom_y = gdispGetHeight() - 100;
    
    // MQTT connection status
    bool mqtt_connected = makapix_mqtt_is_connected();
    const char *mqtt_status_text = mqtt_connected ? "MQTT: Connected" : "MQTT: Disconnected";
    color_t mqtt_status_color = mqtt_connected ? HTML2COLOR(0x00FF00) : HTML2COLOR(0xFF6666);
    gdispFillStringBox(0, bottom_y, gdispGetWidth(), 30, mqtt_status_text,
                     gdispOpenFont("* DejaVu Sans 16"), mqtt_status_color, GFX_BLACK, gJustifyCenter);
    
    // Local IP address
    char ip_str[48];
    if (app_wifi_get_local_ip(ip_str, sizeof(ip_str)) == ESP_OK) {
        char ip_label[64];
        snprintf(ip_label, sizeof(ip_label), "IP: %s", ip_str);
        gdispFillStringBox(0, bottom_y + 40, gdispGetWidth(), 30, ip_label,
                         gdispOpenFont("* DejaVu Sans 16"), HTML2COLOR(0xAAAAAA), GFX_BLACK, gJustifyCenter);
    }
}

/**
 * @brief Parse ISO 8601 timestamp
 */
static bool parse_iso8601(const char *timestamp, time_t *out_time)
{
    struct tm tm = {0};
    if (sscanf(timestamp, "%d-%d-%dT%d:%d:%dZ",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return false;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    *out_time = mktime(&tm);
    return (*out_time != -1);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t ugfx_ui_init(void)
{
    ESP_LOGD(TAG, "µGFX UI system ready");
    return ESP_OK;
}

void ugfx_ui_deinit(void)
{
    if (s_ugfx_initialized) {
        gfxDeinit();
        s_ugfx_initialized = false;
    }
    
    s_ui_active = false;
    s_expires_time = 0;
    memset(s_current_code, 0, sizeof(s_current_code));
    memset(s_status_message, 0, sizeof(s_status_message));
    s_ui_mode = UI_MODE_NONE;
    ugfx_framebuffer_ptr = NULL;
}

esp_err_t ugfx_ui_show_info_screen(void)
{
    s_ui_mode = UI_MODE_INFO_SCREEN;
    s_ui_active = true;
    play_scheduler_pause_auto_swap();

    ESP_LOGD(TAG, "Info screen UI activated");
    return ESP_OK;
}

void ugfx_ui_hide_info_screen(void)
{
    if (s_ui_mode == UI_MODE_INFO_SCREEN) {
        s_ui_active = false;
        s_ui_mode = UI_MODE_NONE;
        play_scheduler_resume_auto_swap();

        ESP_LOGD(TAG, "Info screen UI deactivated");
    }
}

esp_err_t ugfx_ui_show_provisioning_status(const char *status_message)
{
    if (!status_message) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_status_message, status_message, sizeof(s_status_message) - 1);
    s_status_message[sizeof(s_status_message) - 1] = '\0';
    s_ui_mode = UI_MODE_STATUS;
    s_ui_active = true;
    memset(s_current_code, 0, sizeof(s_current_code)); // Clear code when showing status
    
    ESP_LOGD(TAG, "Provisioning status UI activated: %s", status_message);
    return ESP_OK;
}

esp_err_t ugfx_ui_show_captive_ap_info(void)
{
    s_ui_mode = UI_MODE_CAPTIVE_AP_INFO;
    s_ui_active = true;
    memset(s_current_code, 0, sizeof(s_current_code));
    memset(s_status_message, 0, sizeof(s_status_message));
    
    ESP_LOGD(TAG, "Captive AP info UI activated");
    return ESP_OK;
}

esp_err_t ugfx_ui_show_connectivity_error(void)
{
    s_ui_mode = UI_MODE_CONNECTIVITY_ERROR;
    s_ui_active = true;
    memset(s_current_code, 0, sizeof(s_current_code));
    memset(s_status_message, 0, sizeof(s_status_message));

    ESP_LOGD(TAG, "Connectivity error UI activated");
    return ESP_OK;
}

esp_err_t ugfx_ui_show_registration(const char *code, const char *expires_at)
{
    if (!code || !expires_at) {
        return ESP_ERR_INVALID_ARG;
    }

    // Parse expiration time
    if (!parse_iso8601(expires_at, &s_expires_time)) {
        ESP_LOGW(TAG, "Failed to parse expiration time, using default 15 minutes");
        s_expires_time = time(NULL) + 900;
    }

    strncpy(s_current_code, code, sizeof(s_current_code) - 1);
    s_ui_mode = UI_MODE_REGISTRATION;
    s_ui_active = true;
    
    ESP_LOGD(TAG, "Registration UI activated: code=%s", code);
    return ESP_OK;
}

void ugfx_ui_hide_registration(void)
{
    s_ui_active = false;
    s_expires_time = 0;
    s_ui_mode = UI_MODE_NONE;
    memset(s_current_code, 0, sizeof(s_current_code));
    memset(s_status_message, 0, sizeof(s_status_message));
    
    ESP_LOGD(TAG, "Registration UI deactivated");
}

esp_err_t ugfx_ui_show_ota_progress(const char *version_from, const char *version_to)
{
    // Store version info
    if (version_from) {
        strncpy(s_ota_version_from, version_from, sizeof(s_ota_version_from) - 1);
        s_ota_version_from[sizeof(s_ota_version_from) - 1] = '\0';
    } else {
        s_ota_version_from[0] = '\0';
    }
    
    if (version_to) {
        strncpy(s_ota_version_to, version_to, sizeof(s_ota_version_to) - 1);
        s_ota_version_to[sizeof(s_ota_version_to) - 1] = '\0';
    } else {
        s_ota_version_to[0] = '\0';
    }
    
    s_ota_progress = 0;
    strncpy(s_ota_status_text, "Preparing...", sizeof(s_ota_status_text) - 1);
    s_ui_mode = UI_MODE_OTA_PROGRESS;
    s_ui_active = true;
    
    ESP_LOGD(TAG, "OTA progress UI activated: %s -> %s", 
             version_from ? version_from : "?", 
             version_to ? version_to : "?");
    return ESP_OK;
}

void ugfx_ui_update_ota_progress(int percent, const char *status_text)
{
    s_ota_progress = percent;
    if (percent < 0) s_ota_progress = 0;
    if (percent > 100) s_ota_progress = 100;
    
    if (status_text) {
        strncpy(s_ota_status_text, status_text, sizeof(s_ota_status_text) - 1);
        s_ota_status_text[sizeof(s_ota_status_text) - 1] = '\0';
    }
    
    ESP_LOGD(TAG, "OTA progress: %d%% - %s", s_ota_progress, s_ota_status_text);
}

void ugfx_ui_hide_ota_progress(void)
{
    if (s_ui_mode == UI_MODE_OTA_PROGRESS) {
        s_ui_active = false;
        s_ui_mode = UI_MODE_NONE;
        s_ota_progress = 0;
        s_ota_status_text[0] = '\0';
        s_ota_version_from[0] = '\0';
        s_ota_version_to[0] = '\0';
        
        ESP_LOGD(TAG, "OTA progress UI deactivated");
    }
}

esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent)
{
    // Don't let channel messages overwrite the info screen
    if (s_ui_mode == UI_MODE_INFO_SCREEN) {
        ESP_LOGD(TAG, "Channel message ignored: info screen is active");
        return ESP_ERR_INVALID_STATE;
    }

    if (channel_name) {
        strncpy(s_channel_name, channel_name, sizeof(s_channel_name) - 1);
        s_channel_name[sizeof(s_channel_name) - 1] = '\0';
    } else {
        s_channel_name[0] = '\0';
    }
    
    if (message) {
        strncpy(s_channel_message, message, sizeof(s_channel_message) - 1);
        s_channel_message[sizeof(s_channel_message) - 1] = '\0';
    } else {
        s_channel_message[0] = '\0';
    }
    
    s_channel_progress = progress_percent;
    s_ui_mode = UI_MODE_CHANNEL_MESSAGE;
    s_ui_active = true;
    
    ESP_LOGD(TAG, "Channel message UI activated: %s - %s (%d%%)", 
             s_channel_name, s_channel_message, s_channel_progress);
    return ESP_OK;
}

void ugfx_ui_hide_channel_message(void)
{
    if (s_ui_mode == UI_MODE_CHANNEL_MESSAGE) {
        s_ui_active = false;
        s_ui_mode = UI_MODE_NONE;
        s_channel_name[0] = '\0';
        s_channel_message[0] = '\0';
        s_channel_progress = -1;
        
        ESP_LOGD(TAG, "Channel message UI deactivated");
    }
}

gBool ugfx_ui_is_active(void)
{
    return s_ui_active ? gTrue : gFalse;
}

int ugfx_ui_render_to_buffer(uint8_t *buffer, size_t stride)
{
    if (!buffer) {
        return -1;
    }

    // Initialize µGFX if needed, or update framebuffer pointer
    esp_err_t err = ugfx_ui_init_gfx(buffer, stride);
    if (err != ESP_OK) {
        return -1;
    }

    // If UI not active, just clear to black
    if (!s_ui_active) {
        gdispClear(GFX_BLACK);
        return 100;
    }

    // Show appropriate screen based on UI mode
    switch (s_ui_mode) {
        case UI_MODE_STATUS:
            ugfx_ui_draw_status();
            return 100;
            
        case UI_MODE_CAPTIVE_AP_INFO:
            ugfx_ui_draw_captive_ap_info();
            return 100;

        case UI_MODE_CONNECTIVITY_ERROR:
            ugfx_ui_draw_connectivity_error();
            return 100;

        case UI_MODE_INFO_SCREEN:
            ugfx_ui_draw_info_screen();
            return 500;

        case UI_MODE_OTA_PROGRESS:
            ugfx_ui_draw_ota_progress();
            return 50;  // Faster refresh for smooth progress updates
            
        case UI_MODE_REGISTRATION:
            if (s_current_code[0] != '\0') {
                // Calculate remaining time
                time_t now;
                time(&now);
                
                // Check if expiration time is valid (not zero/uninitialized)
                if (s_expires_time == 0) {
                    // Expiration time not set yet - show default 15 minutes
                    int32_t remaining_secs = 900;  // 15 minutes
                    ugfx_ui_draw_layout(remaining_secs);
                    return 100;
                }
                
                int32_t remaining_secs = (int32_t)(s_expires_time - now);
                
                // Clamp to reasonable range to prevent overflow issues
                if (remaining_secs > 3600) {
                    remaining_secs = 3600;  // Cap at 1 hour
                }
                
                // Auto-exit provisioning when code expires
                if (remaining_secs <= 0) {
                    ESP_LOGD(TAG, "Registration code expired, automatically exiting provisioning");
                    makapix_cancel_provisioning();
                    // Draw black screen while transitioning out
                    gdispClear(GFX_BLACK);
                    return 100;
                }
                
                ugfx_ui_draw_layout(remaining_secs);
                return 100;
            }
            break;
            
        case UI_MODE_CHANNEL_MESSAGE:
            ugfx_ui_draw_channel_message();
            return 100;
            
        default:
            break;
    }

    // Fallback: clear to black
    gdispClear(GFX_BLACK);
    return 100;
}

esp_err_t ugfx_ui_set_rotation(screen_rotation_t rotation)
{
    gOrientation ugfx_orientation;
    
    // Map screen_rotation_t to µGFX gOrientation
    // Note: µGFX uses CCW convention, our rotation uses CW convention
    // - gOrientation90 = 90° CCW (270° CW) → maps to ROTATION_270
    // - gOrientation270 = 270° CCW (90° CW) → maps to ROTATION_90
    switch (rotation) {
        case ROTATION_0:
            ugfx_orientation = gOrientation0;
            break;
        case ROTATION_90:
            ugfx_orientation = gOrientation270;  // 90° CW = 270° CCW
            break;
        case ROTATION_180:
            ugfx_orientation = gOrientation180;
            break;
        case ROTATION_270:
            ugfx_orientation = gOrientation90;  // 270° CW = 90° CCW
            break;
        default:
            ESP_LOGE(TAG, "Invalid rotation angle: %d", rotation);
            return ESP_ERR_INVALID_ARG;
    }
    
    // Store for later if µGFX not yet initialized
    s_pending_orientation = ugfx_orientation;
    
    // Apply orientation if µGFX is initialized
    if (s_ugfx_initialized) {
        gdispSetOrientation(ugfx_orientation);
        ESP_LOGD(TAG, "µGFX orientation set to %d degrees", rotation);
    } else {
        ESP_LOGD(TAG, "µGFX not initialized yet, orientation %d pending", rotation);
    }
    
    return ESP_OK;
}
