# µGFX Component for ESP-IDF
# Minimal configuration for text rendering only

idf_component_register(
    SRCS
        # Core GFX
        "src/gfx.c"
        # GOS (FreeRTOS)
        "src/gos/gos_freertos.c"
        "src/gos/gos_x_threads.c"
        "src/gos/gos_x_heap.c"
        # GDRIVER
        "src/gdriver/gdriver.c"
        # GDISP
        "src/gdisp/gdisp.c"
        "src/gdisp/gdisp_fonts.c"
        # mcufont (font rendering)
        "src/gdisp/mcufont/mf_encoding.c"
        "src/gdisp/mcufont/mf_font.c"
        "src/gdisp/mcufont/mf_justify.c"
        "src/gdisp/mcufont/mf_kerning.c"
        "src/gdisp/mcufont/mf_rlefont.c"
        "src/gdisp/mcufont/mf_bwfont.c"
        "src/gdisp/mcufont/mf_scaledfont.c"
        "src/gdisp/mcufont/mf_wordwrap.c"
        # Fonts are included via fonts.h, not compiled separately
        # Framebuffer driver
        "drivers/gdisp/framebuffer/gdisp_lld_framebuffer.c"
    INCLUDE_DIRS
        "."
        "src"
        "src/gos"
        "src/gdisp"
        "src/gdisp/mcufont"
        "src/gdisp/fonts"
        "src/gdriver"
        "drivers/gdisp/framebuffer"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        freertos
)

# Add compiler definitions
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GFX_USE_OS_FREERTOS=GFXON
    GFX_OS_NO_INIT=GFXON
    GFX_USE_GDISP=GFXON
    GDISP_NEED_TEXT=GFXON
    GDISP_NEED_VALIDATION=GFXON
    GDISP_INCLUDE_FONT_DEJAVUSANS16=GFXON
    GDISP_INCLUDE_FONT_DEJAVUSANS24=GFXON
    GDISP_INCLUDE_FONT_DEJAVUSANS32=GFXON
)

