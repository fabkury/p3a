/**
 * @file p3a_board.h
 * @brief P3A Board Abstraction Layer - EP44B Implementation
 * 
 * This header defines the standard interface that all P3A board implementations
 * must provide. The application code includes this header and uses these
 * definitions/functions regardless of which board is targeted.
 * 
 * Board: ESP32-P4-WIFI6-Touch-LCD-4B (EP44B)
 * Display: 720x720 RGB888 MIPI-DSI
 */

#ifndef P3A_BOARD_H
#define P3A_BOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define P3A_BOARD_NAME          "EP44B"
#define P3A_BOARD_FULL_NAME     "ESP32-P4-WIFI6-Touch-LCD-4B"

// ============================================================================
// DISPLAY CONFIGURATION (compile-time constants for zero overhead)
// ============================================================================

/** Display width in pixels */
#define P3A_DISPLAY_WIDTH       720

/** Display height in pixels */
#define P3A_DISPLAY_HEIGHT      720

/** Bits per pixel (16 for RGB565, 24 for RGB888) */
#if CONFIG_P3A_PIXEL_FORMAT_RGB888
#define P3A_DISPLAY_BPP         24
#define P3A_PIXEL_RGB888        1
#define P3A_PIXEL_RGB565        0
#else
#define P3A_DISPLAY_BPP         16
#define P3A_PIXEL_RGB888        0
#define P3A_PIXEL_RGB565        1
#endif

/** Number of display framebuffers (from BSP config) */
#define P3A_DISPLAY_BUFFERS     CONFIG_BSP_LCD_DPI_BUFFER_NUMS

/** Row stride in bytes (may be >= width * bpp/8 due to alignment) */
#define P3A_ROW_STRIDE          (P3A_DISPLAY_WIDTH * P3A_DISPLAY_BPP / 8)

/** Total framebuffer size in bytes */
#define P3A_BUFFER_BYTES        (P3A_ROW_STRIDE * P3A_DISPLAY_HEIGHT)

// ============================================================================
// CAPABILITY FLAGS (compile-time, from Kconfig)
// ============================================================================

#ifdef CONFIG_P3A_HAS_TOUCH
#define P3A_HAS_TOUCH           1
#else
#define P3A_HAS_TOUCH           0
#endif

#ifdef CONFIG_P3A_HAS_BRIGHTNESS_CONTROL
#define P3A_HAS_BRIGHTNESS      1
#else
#define P3A_HAS_BRIGHTNESS      0
#endif

#ifdef CONFIG_P3A_HAS_WIFI
#define P3A_HAS_WIFI            1
#else
#define P3A_HAS_WIFI            0
#endif

#ifdef CONFIG_P3A_HAS_USB
#define P3A_HAS_USB             1
#else
#define P3A_HAS_USB             0
#endif

#ifdef CONFIG_P3A_HAS_SDCARD
#define P3A_HAS_SDCARD          1
#else
#define P3A_HAS_SDCARD          0
#endif

#ifdef CONFIG_P3A_HAS_BUTTONS
#define P3A_HAS_BUTTONS         1
#else
#define P3A_HAS_BUTTONS         0
#endif

// ============================================================================
// PLAYBACK CONFIGURATION (from main Kconfig, exposed here for convenience)
// ============================================================================

#ifdef CONFIG_P3A_MAX_SPEED_PLAYBACK
#define P3A_MAX_SPEED_PLAYBACK  1
#else
#define P3A_MAX_SPEED_PLAYBACK  0
#endif

// ============================================================================
// REQUIRED FUNCTIONS (every board must implement)
// ============================================================================

/**
 * @brief Initialize board display hardware
 * 
 * Initializes the display controller, allocates framebuffers, and sets up
 * brightness control if available. Must be called before any other
 * p3a_board display functions.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t p3a_board_display_init(void);

/**
 * @brief Get LCD panel handle
 * 
 * @return Panel handle, or NULL if not initialized
 */
esp_lcd_panel_handle_t p3a_board_get_panel(void);

/**
 * @brief Get framebuffer by index
 * 
 * @param index Buffer index (0 to P3A_DISPLAY_BUFFERS-1)
 * @return Pointer to framebuffer, or NULL if invalid index
 */
uint8_t* p3a_board_get_buffer(int index);

/**
 * @brief Get number of available framebuffers
 * 
 * @return Number of framebuffers (typically 2 for double-buffering)
 */
uint8_t p3a_board_get_buffer_count(void);

/**
 * @brief Get actual row stride in bytes
 * 
 * The actual stride may be larger than P3A_ROW_STRIDE if the hardware
 * requires alignment padding.
 * 
 * @return Row stride in bytes
 */
size_t p3a_board_get_row_stride(void);

/**
 * @brief Get actual framebuffer size in bytes
 * 
 * @return Framebuffer size in bytes
 */
size_t p3a_board_get_buffer_bytes(void);

// ============================================================================
// BRIGHTNESS CONTROL (available if P3A_HAS_BRIGHTNESS)
// ============================================================================

/**
 * @brief Set display brightness
 * 
 * @param percent Brightness percentage (0-100), will be clamped
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no brightness control
 */
esp_err_t p3a_board_set_brightness(int percent);

/**
 * @brief Get current display brightness
 * 
 * @return Current brightness percentage (0-100), or 100 if not supported
 */
int p3a_board_get_brightness(void);

/**
 * @brief Adjust brightness by delta
 * 
 * @param delta_percent Change in brightness (can be negative)
 * @return ESP_OK on success
 */
esp_err_t p3a_board_adjust_brightness(int delta_percent);

// ============================================================================
// TOUCH (available if P3A_HAS_TOUCH)
// ============================================================================

#if P3A_HAS_TOUCH
#include "esp_lcd_touch.h"

/**
 * @brief Initialize touch hardware
 * 
 * @param[out] handle Touch handle output
 * @return ESP_OK on success
 */
esp_err_t p3a_board_touch_init(esp_lcd_touch_handle_t *handle);
#endif

// ============================================================================
// BUTTONS (available if P3A_HAS_BUTTONS)
// ============================================================================

#if P3A_HAS_BUTTONS
/**
 * @brief Initialize physical button(s)
 * 
 * Configures the BOOT button GPIO as input with pull-up and sets up
 * an ISR with software debouncing. Button presses emit events on the
 * event bus.
 * 
 * @return ESP_OK on success
 */
esp_err_t p3a_board_button_init(void);
#endif

// ============================================================================
// SD CARD (available if P3A_HAS_SDCARD)
// ============================================================================

#if P3A_HAS_SDCARD
/**
 * @brief Mount SD card filesystem
 * 
 * @return ESP_OK on success
 */
esp_err_t p3a_board_sdcard_mount(void);

/**
 * @brief Unmount SD card filesystem
 * 
 * @return ESP_OK on success
 */
esp_err_t p3a_board_sdcard_unmount(void);

/**
 * @brief Get SD card mount point
 * 
 * @return Mount point path (e.g., "/sdcard")
 */
const char* p3a_board_sdcard_mount_point(void);
#endif

// ============================================================================
// LITTLEFS STORAGE (internal flash)
// ============================================================================

/**
 * @brief Mount LittleFS filesystem for web UI assets
 *
 * Mounts the LittleFS partition at /webui for web UI and configuration.
 *
 * @return ESP_OK on success
 */
esp_err_t p3a_board_littlefs_mount(void);

/**
 * @brief Check if LittleFS is mounted
 *
 * @return true if mounted
 */
bool p3a_board_littlefs_is_mounted(void);

/**
 * @brief Check if web UI partition is healthy
 *
 * Call after mounting to verify partition integrity.
 * If unhealthy, the web UI surrogate will be served.
 *
 * @return true if web UI is usable
 */
bool p3a_board_webui_is_healthy(void);

/**
 * @brief Perform health check on LittleFS partition
 *
 * Checks NVS flags and verifies version.txt exists.
 * Sets needs_recovery flag if partition is unhealthy.
 *
 * @return ESP_OK if healthy, ESP_ERR_NOT_FOUND if unhealthy
 */
esp_err_t p3a_board_littlefs_check_health(void);

// ============================================================================
// LEGACY COMPATIBILITY MACROS
// These provide backward compatibility during migration
// TODO: Remove these after full migration
// ============================================================================

#define EXAMPLE_LCD_H_RES           P3A_DISPLAY_WIDTH
#define EXAMPLE_LCD_V_RES           P3A_DISPLAY_HEIGHT
#define EXAMPLE_LCD_BUF_NUM         P3A_DISPLAY_BUFFERS
#define EXAMPLE_LCD_BIT_PER_PIXEL   P3A_DISPLAY_BPP
#define EXAMPLE_LCD_BUF_LEN         P3A_BUFFER_BYTES

// Note: BSP_LCD_H_RES and BSP_LCD_V_RES are provided by the vendor BSP
// and should NOT be redefined here to avoid conflicts

// Legacy pixel format macros (for code using #if CONFIG_LCD_PIXEL_FORMAT_*)
#if P3A_PIXEL_RGB888
#define CONFIG_LCD_PIXEL_FORMAT_RGB888 1
#undef  CONFIG_LCD_PIXEL_FORMAT_RGB565
#else
#define CONFIG_LCD_PIXEL_FORMAT_RGB565 1
#undef  CONFIG_LCD_PIXEL_FORMAT_RGB888
#endif

#ifdef CONFIG_P3A_MAX_SPEED_PLAYBACK
#define APP_LCD_MAX_SPEED_PLAYBACK_ENABLED 1
#else
#define APP_LCD_MAX_SPEED_PLAYBACK_ENABLED 0
#endif

#ifdef __cplusplus
}
#endif

#endif // P3A_BOARD_H

