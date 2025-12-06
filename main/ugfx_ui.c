#include "ugfx_ui.h"
#include "esp_log.h"
#include "app_lcd.h"
#include "app_wifi.h"
#include "bsp/display.h"
#include "makapix_mqtt.h"
#include "makapix.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Include µGFX headers
#include "gfx.h"
#include "gdisp/gdisp.h"

static const char *TAG = "ugfx_ui";

// UI mode enumeration
typedef enum {
    UI_MODE_NONE,              // No UI active
    UI_MODE_STATUS,            // Provisioning status
    UI_MODE_REGISTRATION,      // Registration code display
    UI_MODE_CAPTIVE_AP_INFO    // Captive portal setup info
} ui_mode_t;

// UI state
static bool s_ui_active = false;
static bool s_ugfx_initialized = false;
static time_t s_expires_time = 0;
static char s_current_code[16] = {0};
static char s_status_message[128] = {0};
static ui_mode_t s_ui_mode = UI_MODE_NONE;
static gOrientation s_pending_orientation = gOrientation0;  // Orientation to apply when µGFX inits

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

    ESP_LOGI(TAG, "Initializing µGFX with framebuffer %p, dimensions %dx%d, stride=%zu", 
             framebuffer, ugfx_screen_width, ugfx_screen_height, stride);

    gfxInit();
    s_ugfx_initialized = true;
    
    // Apply any pending orientation that was set before initialization
    if (s_pending_orientation != gOrientation0) {
        gdispSetOrientation(s_pending_orientation);
        ESP_LOGI(TAG, "Applied pending orientation: %d", s_pending_orientation);
    }
    
    ESP_LOGI(TAG, "µGFX initialized: display size %dx%d", gdispGetWidth(), gdispGetHeight());
    return ESP_OK;
}

/**
 * @brief Draw the captive portal AP info screen
 */
static void ugfx_ui_draw_captive_ap_info(void)
{
    gdispClear(GFX_BLACK);

    // Title
    gdispFillStringBox(0, 60, gdispGetWidth(), 30, "WiFi Setup Instructions",
                     gdispOpenFont("* DejaVu Sans 24"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Instructions (multi-line, smaller font)
    int y_pos = 120;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 30, "1. Connect to the WiFi network:",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    
    y_pos += 40;
    // Using CONFIG_ESP_AP_SSID directly (EXAMPLE_ESP_AP_SSID wrapper is local to app_wifi.c)
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 30, CONFIG_ESP_AP_SSID,
                     gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0x00FF00), GFX_BLACK, gJustifyCenter);
    
    y_pos += 50;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 30, "2. Open your web browser",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    
    y_pos += 40;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 30, "3. Go to: http://p3a.local",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    
    y_pos += 40;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 30, "or http://192.168.4.1",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
    
    y_pos += 50;
    gdispFillStringBox(0, y_pos, gdispGetWidth(), 30, "4. Enter your WiFi credentials",
                     gdispOpenFont("* DejaVu Sans 20"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);
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
    gdispFillStringBox(0, gdispGetHeight()/2 + 35, gdispGetWidth(), 35, "https://dev.makapix.club/",
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
    ESP_LOGI(TAG, "µGFX UI system ready");
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
    
    ESP_LOGI(TAG, "Provisioning status UI activated: %s", status_message);
    return ESP_OK;
}

esp_err_t ugfx_ui_show_captive_ap_info(void)
{
    s_ui_mode = UI_MODE_CAPTIVE_AP_INFO;
    s_ui_active = true;
    memset(s_current_code, 0, sizeof(s_current_code));
    memset(s_status_message, 0, sizeof(s_status_message));
    
    ESP_LOGI(TAG, "Captive AP info UI activated");
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
    
    ESP_LOGI(TAG, "Registration UI activated: code=%s", code);
    return ESP_OK;
}

void ugfx_ui_hide_registration(void)
{
    s_ui_active = false;
    s_expires_time = 0;
    s_ui_mode = UI_MODE_NONE;
    memset(s_current_code, 0, sizeof(s_current_code));
    memset(s_status_message, 0, sizeof(s_status_message));
    
    ESP_LOGI(TAG, "Registration UI deactivated");
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
                    ESP_LOGI(TAG, "Registration code expired, automatically exiting provisioning");
                    makapix_cancel_provisioning();
                    // Draw black screen while transitioning out
                    gdispClear(GFX_BLACK);
                    return 100;
                }
                
                ugfx_ui_draw_layout(remaining_secs);
                return 100;
            }
            break;
            
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
        ESP_LOGI(TAG, "µGFX orientation set to %d degrees", rotation);
    } else {
        ESP_LOGI(TAG, "µGFX not initialized yet, orientation %d pending", rotation);
    }
    
    return ESP_OK;
}
