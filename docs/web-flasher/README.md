# p3a Web Flasher — Technical Investigation

## Status: ❌ Not Functional (December 2025)

Browser-based flashing for ESP32-P4 devices **does not work** due to bugs in Espressif's esptool-js library. This document preserves the full technical investigation for future reference.

---

## Summary

We attempted to implement a GitHub Pages-based web flasher using [esptool-js](https://github.com/espressif/esptool-js). Despite multiple approaches including using an unreleased PR with Uint8Array support, all attempts resulted in corrupted firmware data being written to the device.

**Root cause:** esptool-js corrupts binary data during the compressed write operation for ESP32-P4 devices. The corruption occurs inside the library, after our code hands off the data.

---

## Investigation Timeline

### Attempt 1: Official esptool-js (v0.5.7) with Binary Strings

**Approach:** Use the official npm release with binary string data.

**Result:** ❌ Failed
```
Warning: Image file at 0x2000 doesn't look like an image file, so not changing any flash settings.
```
The library couldn't parse the ESP32-P4 bootloader header. Device failed to boot.

### Attempt 2: ISO-8859-1 String Encoding

**Approach:** Convert Uint8Array to binary string using ISO-8859-1 (Latin-1) encoding for 1:1 byte mapping.

**Result:** ❌ Failed
```
Checksum failure. Calculated 0xc6 stored 0xcc
```
Bootloader data was corrupted during the flash process.

### Attempt 3: PR #226 with Native Uint8Array Support

**Approach:** Build esptool-js from [PR #226](https://github.com/espressif/esptool-js/pull/226) which adds native Uint8Array support.

**Result:** ❌ Failed

We verified the data was correct **before** calling `writeFlash()`:
```
p3a.bin: 1807904 bytes, first 8: [e9 07 02 5f 0e 04 f0 4f], magic: 0xe9 ✓
```

But the device failed to boot **after** flashing:
```
E (119) esp_image: image at 0x20000 has invalid magic byte (nothing flashed here?)
```

The corruption occurs inside esptool-js during the compressed write.

### Attempt 4: Disable Compression

**Approach:** Set `compress: false` in flash options.

**Result:** ❌ Not supported
```
Flash failed: Yet to handle Non Compressed writes
```
PR #226 only implements Uint8Array support for compressed writes.

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

### Flash Addresses

| File | Address | Purpose |
|------|---------|---------|
| bootloader.bin | 0x2000 | Second stage bootloader |
| partition-table.bin | 0x8000 | Flash partition layout |
| ota_data_initial.bin | 0x10000 | OTA boot selection |
| p3a.bin | 0x20000 | Main application |
| storage.bin | 0x1020000 | Persistent storage |
| network_adapter.bin | 0x1120000 | ESP32-C6 Wi-Fi co-processor firmware |

### Working Alternative

The same firmware flashes correctly with Python esptool:
```bash
python -m esptool --chip esp32p4 -p COM5 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

---

## Files in This Directory

| File | Purpose |
|------|---------|
| `index.html` | Web flasher UI (non-functional) |
| `flasher.js` | JavaScript flashing logic |
| `lib/esptool-bundle.js` | Custom build from PR #226 |
| `lib/README.md` | Build instructions for esptool-js |
| `manifest.json` | Firmware metadata |
| `ESPRESSIF_REPORT.md` | Report to post on GitHub issues |
| `p3a-logo.png` | UI asset |
| `favicon.png` | UI asset |

---

## Issue Tracking

- **Issue #229:** [ESP32-P4 Flash Settings Not Applied](https://github.com/espressif/esptool-js/issues/229)
- **PR #226:** [use uint8array instead of string for write flash command](https://github.com/espressif/esptool-js/pull/226)

---

## Future

If Espressif fixes the data corruption in esptool-js for ESP32-P4, the web flasher can be re-enabled by:

1. Updating `lib/esptool-bundle.js` to a fixed version
2. Testing that firmware data survives the write process
3. Re-adding web flasher references to `docs/flash-p3a.md`

The implementation is complete and ready — it just needs the underlying library to work correctly.

---

*Last updated: December 20, 2025*
