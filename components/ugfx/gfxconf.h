/**
 * µGFX Configuration for ESP32-P4
 * Minimal configuration for text rendering only
 */

#ifndef _GFXCONF_H
#define _GFXCONF_H

///////////////////////////////////////////////////////////////////////////
// GOS - FreeRTOS                                                       //
///////////////////////////////////////////////////////////////////////////
#define GFX_USE_OS_FREERTOS              GFXON
#define GFX_OS_NO_INIT                   GFXON   // FreeRTOS already running

///////////////////////////////////////////////////////////////////////////
// GDISP                                                                 //
///////////////////////////////////////////////////////////////////////////
#define GFX_USE_GDISP                    GFXON

#define GDISP_NEED_VALIDATION            GFXON
#define GDISP_NEED_CLIP                  GFXON
#define GDISP_NEED_TEXT                  GFXON
#define GDISP_NEED_TEXT_WORDWRAP         GFXOFF
#define GDISP_NEED_TEXT_BOXPADLR         1
#define GDISP_NEED_TEXT_BOXPADTB         1
#define GDISP_NEED_ANTIALIAS             GFXOFF
#define GDISP_NEED_UTF8                  GFXOFF
#define GDISP_NEED_TEXT_KERNING          GFXOFF

// Include fonts
#define GDISP_INCLUDE_FONT_DEJAVUSANS16  GFXON
#define GDISP_INCLUDE_FONT_DEJAVUSANS24  GFXON
#define GDISP_INCLUDE_FONT_DEJAVUSANS32  GFXON

// Display configuration
#define GDISP_TOTAL_DISPLAYS             1

// Pixel format will be set in board_framebuffer.h based on config

#endif /* _GFXCONF_H */


