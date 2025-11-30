/*
 * Board file for ESP32-P4 DPI panel framebuffer
 * This file interfaces µGFX with the framebuffer provided by app_lcd
 */

#ifndef _BOARD_FRAMEBUFFER_H
#define _BOARD_FRAMEBUFFER_H

#include "sdkconfig.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Update the framebuffer pointer at runtime
 * 
 * This allows switching which buffer µGFX draws to without reinitializing.
 * Call this before each frame when using multiple framebuffers.
 * 
 * @param pixels Pointer to the new framebuffer
 * @param linelen Row stride in bytes
 */
void gdisp_lld_set_framebuffer(void *pixels, gCoord linelen);

#ifdef __cplusplus
}
#endif

// Set pixel format based on ESP-IDF config
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
    #define GDISP_LLD_PIXELFORMAT    GDISP_PIXELFORMAT_RGB888
#elif CONFIG_BSP_LCD_COLOR_FORMAT_RGB565
    #define GDISP_LLD_PIXELFORMAT    GDISP_PIXELFORMAT_RGB565
#else
    #error "Unsupported LCD color format"
#endif

#ifdef GDISP_DRIVER_VMT

// Forward declarations
struct GDisplay;
// fbInfo is defined in gdisp_lld_framebuffer.c before this header is included

// External variables set by ugfx_ui.c before gfxInit()
// Using int instead of gCoord since gCoord may not be defined yet
extern void *ugfx_framebuffer_ptr;
extern int ugfx_screen_width;
extern int ugfx_screen_height;
extern size_t ugfx_line_stride;

static void board_init(struct GDisplay *g, struct fbInfo *fbi) {
    // Set display dimensions from external variables
    // Note: gCoord should be available here since this is compiled after gdisp_driver.h is included
    g->g.Width = (gCoord)ugfx_screen_width;
    g->g.Height = (gCoord)ugfx_screen_height;
    g->g.Backlight = 100;
    g->g.Contrast = 50;
    
    // Set framebuffer pointer and line stride
    fbi->linelen = ugfx_line_stride;
    fbi->pixels = ugfx_framebuffer_ptr;
}

// No hardware flush needed - DMA handles it
// #define GDISP_HARDWARE_FLUSH is not defined

#if GDISP_NEED_CONTROL
    static void board_backlight(GDisplay *g, gU8 percent) {
        // Backlight control handled by app_lcd
        (void)g;
        (void)percent;
    }

    static void board_contrast(GDisplay *g, gU8 percent) {
        // Contrast control not supported
        (void)g;
        (void)percent;
    }

    static void board_power(GDisplay *g, gPowermode pwr) {
        // Power control handled by app_lcd
        (void)g;
        (void)pwr;
    }
#endif

#endif /* GDISP_DRIVER_VMT */

#endif /* _BOARD_FRAMEBUFFER_H */


