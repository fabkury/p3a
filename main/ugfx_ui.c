#include "ugfx_ui.h"
#include "esp_log.h"
#include "app_lcd.h"
#include "bsp/display.h"
#include "makapix_mqtt.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Include µGFX headers
#include "gfx.h"
#include "gdisp/gdisp.h"

static const char *TAG = "ugfx_ui";

// UI state
static bool s_ui_active = false;
static bool s_ugfx_initialized = false;
static time_t s_expires_time = 0;
static char s_current_code[16] = {0};
static char s_status_message[128] = {0};
static bool s_show_status = false;

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
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing µGFX with framebuffer %p, dimensions %dx%d, stride=%zu", 
             framebuffer, ugfx_screen_width, ugfx_screen_height, stride);

    gfxInit();
    s_ugfx_initialized = true;
    
    ESP_LOGI(TAG, "µGFX initialized: display size %dx%d", gdispGetWidth(), gdispGetHeight());
    return ESP_OK;
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
    gdispFillStringBox(0, 80, gdispGetWidth(), 30, "REGISTER PLAYER",
                     gdispOpenFont("* DejaVu Sans 24"), GFX_WHITE, GFX_BLACK, gJustifyCenter);

    // Registration code (large, green)
    gdispFillStringBox(0, gdispGetHeight()/2 - 40, gdispGetWidth(), 50, s_current_code,
                     gdispOpenFont("* DejaVu Sans 32"), HTML2COLOR(0x00FF00), GFX_BLACK, gJustifyCenter);

    // Instructions
    gdispFillStringBox(0, gdispGetHeight()/2 + 40, gdispGetWidth(), 50, "Enter this code at: https://dev.makapix.club/",
                     gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0xCCCCCC), GFX_BLACK, gJustifyCenter);

    // Countdown timer
    if (remaining_secs <= 0) {
        gdispFillStringBox(0, gdispGetHeight() - 120, gdispGetWidth(), 30, "Expired",
                         gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0xFFFF00), GFX_BLACK, gJustifyCenter);
    } else {
        int minutes = remaining_secs / 60;
        int seconds = remaining_secs % 60;
        char timer_text[32];
        snprintf(timer_text, sizeof(timer_text), "Expires in %02d:%02d", minutes, seconds);
        gdispFillStringBox(0, gdispGetHeight() - 120, gdispGetWidth(), 30, timer_text,
                         gdispOpenFont("* DejaVu Sans 24"), HTML2COLOR(0xFFFF00), GFX_BLACK, gJustifyCenter);
    }

    // MQTT connection status
    bool mqtt_connected = makapix_mqtt_is_connected();
    const char *mqtt_status_text = mqtt_connected ? "MQTT: Connected" : "MQTT: Disconnected";
    color_t mqtt_status_color = mqtt_connected ? HTML2COLOR(0x00FF00) : HTML2COLOR(0xFF0000);
    gdispFillStringBox(0, gdispGetHeight() - 80, gdispGetWidth(), 30, mqtt_status_text,
                     gdispOpenFont("* DejaVu Sans 24"), mqtt_status_color, GFX_BLACK, gJustifyCenter);
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
    s_show_status = false;
    ugfx_framebuffer_ptr = NULL;
}

esp_err_t ugfx_ui_show_provisioning_status(const char *status_message)
{
    if (!status_message) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_status_message, status_message, sizeof(s_status_message) - 1);
    s_status_message[sizeof(s_status_message) - 1] = '\0';
    s_show_status = true;
    s_ui_active = true;
    memset(s_current_code, 0, sizeof(s_current_code)); // Clear code when showing status
    
    ESP_LOGI(TAG, "Provisioning status UI activated: %s", status_message);
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
    s_show_status = false; // Switch from status to code display
    s_ui_active = true;
    
    ESP_LOGI(TAG, "Registration UI activated: code=%s", code);
    return ESP_OK;
}

void ugfx_ui_hide_registration(void)
{
    s_ui_active = false;
    s_expires_time = 0;
    s_show_status = false;
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

    // Show status message if in status mode
    if (s_show_status) {
        ugfx_ui_draw_status();
        return 100;
    }

    // Show registration code if available
    if (s_current_code[0] != '\0') {
        // Calculate remaining time and draw UI
        time_t now;
        time(&now);
        int32_t remaining_secs = (int32_t)(s_expires_time - now);
        ugfx_ui_draw_layout(remaining_secs);
        return 100;
    }

    // Fallback: clear to black
    gdispClear(GFX_BLACK);
    return 100;
}
