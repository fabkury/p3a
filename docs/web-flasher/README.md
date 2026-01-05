# p3a Web Flasher

## Status: Working (January 2026)

Browser-based flashing for ESP32-P4 devices is now functional using esptool-js from the `enhance/write-flash-array-buffer` branch, which fixes the data corruption issues that affected earlier versions.

**Live URL:** https://fabkury.github.io/p3a/web-flasher/

---

## Features

- Select firmware version from GitHub Releases
- Flash all firmware files in a single operation
- Real-time progress tracking per file
- Works in Chrome 89+ and Edge 89+

---

## How It Works

1. **Select Firmware:** Choose a release version from the dropdown
2. **Connect Device:** Click "Connect" and select your ESP32-P4 from the port picker
3. **Flash:** Click "Flash Device" to begin the flashing process

The flasher downloads all required files from the selected GitHub Release and flashes them to the device using the addresses specified in `flash_args`.

---

## Technical Details

### Flash Configuration

| Setting | Value |
|---------|-------|
| Chip | ESP32-P4 |
| Flash Mode | DIO |
| Flash Frequency | 80 MHz |
| Flash Size | 32 MB |
| Baud Rate | 460800 |

### Files in This Directory

| File | Purpose |
|------|---------|
| `index.html` | Web flasher UI and logic |
| `esptool-bundle.js` | esptool-js library (from `enhance/write-flash-array-buffer` branch) |
| `p3a-logo.png` | UI asset |
| `favicon.png` | UI asset |
| `ESPRESSIF_REPORT.md` | Historical: bug report from December 2025 investigation |

### esptool-js Source

The `esptool-bundle.js` was built from:
- Repository: https://github.com/espressif/esptool-js
- Branch: `enhance/write-flash-array-buffer`
- PR: https://github.com/espressif/esptool-js/pull/226

This branch provides native Uint8Array support which fixes the data corruption that affected ESP32-P4 flashing.

---

## Troubleshooting

- **No device found:** Use a USB-C data cable (not charge-only). Connect to the USB-OTG port.
- **Connection fails:** Hold the BOOT button while clicking Connect.
- **Browser not supported:** Use Chrome or Edge (Firefox and Safari don't support WebSerial).

For persistent issues, use the [command-line flash guide](../flash-p3a.md).

---

*Last updated: January 2026*
