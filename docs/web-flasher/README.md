# p3a Web Flasher

Browser-based flashing for the p3a board, served from GitHub Pages.

**Live URL:** https://fabkury.github.io/p3a/web-flasher/

## How it works

1. **Select firmware:** the dropdown lists GitHub Releases from 1.0.0 onward
   (`CONFIG.MIN_MAJOR_VERSION` in `index.html`); the newest is preselected.
2. **Connect:** click "Connect" and pick the ESP32-P4 in the port picker.
3. **Flash:** "Flash Device" writes every file at the addresses in that
   release's `flash_args`.

A 2.x release erases the device: its `flash_args` include `nvs-erased.bin`,
blank bytes written over the settings partition, so that a device that ran
1.x starts fresh. The page says so when a 2.x release is selected, and the
Flash button stays off until the box under the warning is ticked. A 2.x
device updates itself from its own Update page; the flasher is for new
devices, for 1.x devices, and for recovery.

GitHub release assets don't support CORS, so the flasher loads firmware from
the same origin: `./firmware/{tag}/`. The `publish-firmware-to-pages`
workflow fills `firmware/{tag}/` on `main` whenever a release is published.
Folders for releases older than `MIN_MAJOR_VERSION` were pruned; if you lower
that constant, restore the matching folders first, or those dropdown entries
fail to download.

## Flash configuration

| Setting | Value |
|---------|-------|
| Chip | ESP32-P4 |
| Flash mode | DIO |
| Flash frequency | 80 MHz |
| Flash size | 32 MB |
| Baud rate | 460800 |

## Files

| File | Purpose |
|------|---------|
| `index.html` | Flasher UI and logic |
| `esptool-bundle.js` | esptool-js built from the `enhance/write-flash-array-buffer` branch ([PR #226](https://github.com/espressif/esptool-js/pull/226)); its native `Uint8Array` support fixes the data corruption that affected ESP32-P4 flashing |
| `p3a-logo.png`, `favicon.png` | UI assets |
| `firmware/{tag}/` | Per-release flash files, written by CI |

## Troubleshooting

- **No device found:** use a USB-C data cable (not charge-only) on the USB-OTG port.
- **Connection fails:** hold the BOOT button (the top one, farther from the USB-C ports) while clicking Connect.
- **Browser not supported:** use Chrome or Edge 89+ (Firefox and Safari don't support WebSerial).

For persistent issues, use the [command-line flash guide](../flash-p3a.md).
