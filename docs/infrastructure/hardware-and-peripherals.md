# Hardware and Peripherals

## Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B

### CPU and Memory

- **Main MCU**: Dual-core ESP32-P4 (RISC-V)
- **Wi-Fi/BLE MCU**: ESP32-C6-MINI-1 (managed by ESP-Hosted)
- **PSRAM**: 32MB external SDRAM
- **Flash**: 32MB NOR flash
- **Interconnect**: ESP-P4 ↔ ESP-C6 via SDIO (coordinated by `sdio_bus` component)

### Display

- **Panel**: 720x720 pixel IPS LCD
- **Controller**: ST7703 (via MIPI-DSI)
- **Color depth**: RGB565 or RGB888 (configurable via `P3A_PIXEL_FORMAT` Kconfig choice)
- **Frame buffer**: Triple buffering in PSRAM (720x720x3 bytes each for RGB888)
- **Abstraction**: `p3a_board.h` provides `P3A_DISPLAY_WIDTH` (720), `P3A_DISPLAY_HEIGHT` (720)
- **Brightness**: Software-controlled via `p3a_board_set_brightness()` / `p3a_board_get_brightness()` / `p3a_board_adjust_brightness()`

### Touch Controller

- **Controller**: GT911 capacitive touch
- **Interface**: I2C
- **Features**: 5-point multitouch, gestures
- **Routing**: Touch events are routed through `p3a_touch_router` (in `p3a_core`) based on the current application state — tap left/right for navigation, swipe for brightness, long press for actions

### Buttons

- **BOOT button**: Used for pause/resume toggle
- **Conditional**: Only available when `P3A_HAS_BUTTONS` is defined
- **Implementation**: `p3a_board_button.c` in the `p3a_board_ep44b` component

### Storage (SD Card)

- **Interface**: SDMMC (4-bit mode)
- **Mount point**: `/sdcard` (hardware level, BSP configured)
- **Functions**: `p3a_board_sdcard_mount()`, `p3a_board_sdcard_unmount()`, `p3a_board_sdcard_mount_point()`
- **Bus coordination**: The SDIO bus is shared between WiFi (Slot 1) and SD card (Slot 0). The `sdio_bus` component provides mutex-based coordination to prevent "SDIO slave unresponsive" crashes during concurrent access (e.g., OTA downloads).

### USB

- **Ports**: 2x USB-C (High-Speed used for composite device)
- **TinyUSB stack**: CDC-ACM + Mass Storage + Vendor bulk
- **Conditional**: USB functionality is enabled/disabled via `P3A_USB_MSC_ENABLE` Kconfig option
- **Files**: `app_usb.c`, `usb_descriptors.c` (only compiled when enabled)

### Storage (Internal Flash)

- **LittleFS partition**: `storage` (4 MB)
- **Mount point**: `/webui`
- **Initialization**: `p3a_board_littlefs_mount()` + `p3a_board_littlefs_check_health()`
- **Health check**: `p3a_board_webui_is_healthy()` — verifies the web UI partition is intact
- **Purpose**: Web UI assets (HTML, CSS, JS), version metadata
