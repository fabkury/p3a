# p3a Web Flasher

## Current Status: ⚠️ Unavailable for ESP32-P4

The browser-based web flasher is **not currently functional** for ESP32-P4 devices due to a limitation in Espressif's esptool-js library.

### The Problem

esptool-js fails to recognize the ESP32-P4 bootloader as a valid image file:

```
Warning: Image file at 0x2000 doesn't look like an image file, so not changing any flash settings.
```

This causes the `flashMode`, `flashFreq`, and `flashSize` options to be ignored, resulting in incorrect flash configuration. The device writes data successfully but fails to boot.

### Workaround

Use the command-line esptool instead. See [flash-p3a.md](../flash-p3a.md) for instructions.

### Bug Report

We've documented this issue for Espressif: [ESPTOOL_JS_BUG_REPORT.md](../ESPTOOL_JS_BUG_REPORT.md)

Track the issue at: https://github.com/espressif/esptool-js/issues

---

## Technical Details

### Files in this directory

| File | Purpose |
|------|---------|
| `index.html` | Information page explaining the limitation |
| `manifest.json` | Firmware manifest (for future use) |
| `manifest-local.json` | Local testing manifest (for future use) |
| `firmware/` | Local firmware files for testing (gitignored) |

### Flash Configuration Required for ESP32-P4

| Setting | Value | Description |
|---------|-------|-------------|
| `--flash-mode` | `dio` | Dual I/O mode (required) |
| `--flash-freq` | `80m` | 80 MHz flash frequency |
| `--flash-size` | `32MB` | 32 MB flash size |
| `--force` | (flag) | Required for ESP32-C6 co-processor firmware |

### Flash Addresses

| File | Hex Address | Decimal |
|------|-------------|---------|
| bootloader.bin | 0x2000 | 8192 |
| partition-table.bin | 0x8000 | 32768 |
| ota_data_initial.bin | 0x10000 | 65536 |
| p3a.bin | 0x20000 | 131072 |
| storage.bin | 0x1020000 | 16908288 |
| network_adapter.bin | 0x1120000 | 17956864 |

---

## Future

Once Espressif fixes esptool-js to properly handle ESP32-P4 flash settings, we can re-enable the web flasher. The implementation is ready—it just needs the underlying library to work correctly.
