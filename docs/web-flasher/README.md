# p3a Web Flasher

## Current Status: ✅ Available

The browser-based web flasher is now functional for ESP32-P4 devices! Flash your p3a directly from your browser without installing any software.

**[Launch Web Flasher →](https://fabkury.github.io/p3a/docs/web-flasher/)**

### Requirements

- **Browser**: Chrome, Edge, or Opera (requires Web Serial API)
- **USB cable**: Must be a data cable, not charge-only
- **p3a device**: ESP32-P4 based (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B or compatible)

### How It Works

1. **Connect** — Click "Connect" and select your p3a from the serial port list
2. **Select Firmware** — Choose a release from GitHub or upload your own files
3. **Flash** — Click "Flash Device" and wait ~2 minutes for completion

### Technical Details

The web flasher uses [esptool-js](https://github.com/espressif/esptool-js), Espressif's JavaScript implementation of their flash tool. Recent updates to esptool-js (PR #226) fixed compatibility with ESP32-P4 by using `Uint8Array` instead of strings for binary data, which allows proper parsing of the bootloader image headers.

#### Files in this directory

| File | Purpose |
|------|---------|
| `index.html` | Main web flasher interface |
| `flasher.js` | JavaScript flashing logic using esptool-js |
| `manifest.json` | Firmware manifest with addresses and settings |
| `README.md` | This documentation |

#### Flash Configuration for ESP32-P4

| Setting | Value | Description |
|---------|-------|-------------|
| `flashMode` | `dio` | Dual I/O mode (required for ESP32-P4) |
| `flashFreq` | `80m` | 80 MHz flash frequency |
| `flashSize` | `32MB` | 32 MB flash size |

#### Flash Addresses

| File | Address | Description |
|------|---------|-------------|
| bootloader.bin | 0x2000 | ESP32-P4 second stage bootloader |
| partition-table.bin | 0x8000 | Flash partition layout |
| ota_data_initial.bin | 0x10000 | OTA boot selection |
| p3a.bin | 0x20000 | Main application |
| storage.bin | 0x1020000 | Persistent storage |
| network_adapter.bin | 0x1120000 | ESP32-C6 Wi-Fi co-processor firmware |

---

## Troubleshooting

### Device not detected

- Use a **data USB cable** (many cables are charge-only)
- Try holding the **BOOT button** while connecting the USB cable
- On Windows, install the [CP210x drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) if needed
- Make sure no other application (like Arduino IDE or a serial monitor) is using the port

### Flash fails or device doesn't boot

- Click "Erase All" first, then try flashing again
- Verify all 6 firmware files are loaded (check the firmware status section)
- Confirm your device is actually an ESP32-P4

### Browser shows "Not Supported"

Web Serial API is only available in Chromium-based browsers:
- ✅ Google Chrome
- ✅ Microsoft Edge  
- ✅ Opera
- ❌ Firefox (not supported)
- ❌ Safari (not supported)

---

## Alternative: Command-line flashing

If the web flasher doesn't work for you, use the Python esptool:

```bash
# Install
pip install esptool

# Flash (Windows)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

See [flash-p3a.md](../flash-p3a.md) for detailed instructions.

---

## Development

### Local testing

To test the web flasher locally:

1. Start a local HTTP server in the repo root:
   ```bash
   python -m http.server 8000
   ```

2. Open http://localhost:8000/docs/web-flasher/

3. Place firmware files in a `firmware/` subdirectory (gitignored) or use the ZIP upload feature

### Dependencies

- [esptool-js](https://github.com/espressif/esptool-js) — ESP32 flashing library (loaded from unpkg CDN)
- [JSZip](https://stuk.github.io/jszip/) — ZIP file extraction (loaded from cdnjs)

---

## History

The web flasher was initially disabled due to [esptool-js issue #229](https://github.com/espressif/esptool-js/issues/229), where ESP32-P4 flash settings were not being applied correctly. The fix in [PR #226](https://github.com/espressif/esptool-js/pull/226) (using `Uint8Array` instead of strings) resolved this issue by ensuring proper binary data handling during bootloader image parsing.
