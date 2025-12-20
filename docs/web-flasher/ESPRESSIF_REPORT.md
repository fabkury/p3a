# Report for Espressif: ESP32-P4 Web Flashing Still Fails with PR #226

**Copy this report to:**
- Issue #229: https://github.com/espressif/esptool-js/issues/229
- PR #226: https://github.com/espressif/esptool-js/pull/226

---

## Summary

We tested PR #226 (Uint8Array support) for flashing ESP32-P4 devices. **The data corruption persists.** Firmware data is verified correct immediately before calling `writeFlash()`, but the device fails to boot with "invalid magic byte" errors after flashing.

## Test Setup

- **Device:** ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B)
- **esptool-js:** Custom build from PR #226 branch (`git fetch origin pull/226/head:pr-226`)
- **Browser:** Chrome 131 on Windows 11
- **Flash settings:** `flashMode: "dio"`, `flashFreq: "80m"`, `flashSize: "32MB"`

## Firmware Files

| File | Address | Size | Magic Byte |
|------|---------|------|------------|
| bootloader.bin | 0x2000 | 23,584 bytes | 0xE9 ✓ |
| partition-table.bin | 0x8000 | 3,072 bytes | 0xAA ✓ |
| ota_data_initial.bin | 0x10000 | 8,192 bytes | 0xFF ✓ |
| p3a.bin | 0x20000 | 1,807,904 bytes | 0xE9 ✓ |
| storage.bin | 0x1020000 | 1,048,576 bytes | 0xFF ✓ |
| network_adapter.bin | 0x1120000 | 1,167,040 bytes | 0xE9 ✓ |

## Key Finding: Data Correct Before Write, Corrupted After

We added verification logging immediately before calling `writeFlash()`:

```
[02:05:07] Verifying firmware data...
[02:05:07]   bootloader.bin: 23584 bytes, first 8: [e9 03 02 5f da 9e f2 4f], magic: 0xe9
[02:05:07]   partition-table.bin: 3072 bytes, first 8: [aa 50 01 02 00 90 00 00], magic: 0xaa
[02:05:07]   ota_data_initial.bin: 8192 bytes, first 8: [ff ff ff ff ff ff ff ff], magic: 0xff
[02:05:07]   p3a.bin: 1807904 bytes, first 8: [e9 07 02 5f 0e 04 f0 4f], magic: 0xe9
[02:05:07]   storage.bin: 1048576 bytes, first 8: [ff ff ff ff ff ff ff ff], magic: 0xff
[02:05:07]   network_adapter.bin: 1167040 bytes, first 8: [e9 05 02 20 d4 03 80 40], magic: 0xe9
```

**All magic bytes are correct (0xE9 for app images).** The Uint8Array data is valid.

Flash appears to succeed:
```
[02:05:07] Flash params set to 25f
[02:05:07] Compressed 1807904 bytes to 1031971...
[02:05:08] Writing at 0x20000... (1%)
...
[02:05:34] Wrote 1807904 bytes (1031971 compressed) at 0x20000 in 25.776 seconds.
```

But the device fails to boot:
```
E (119) esp_image: image at 0x20000 has invalid magic byte (nothing flashed here?)
E (127) boot: OTA app partition slot 0 is not bootable
```

## What We Tested

| Approach | Result |
|----------|--------|
| Official esptool-js 0.5.7 with binary strings | ❌ "Image doesn't look like an image file" warning, data corrupted |
| Official esptool-js 0.5.7 with ISO-8859-1 string encoding | ❌ Checksum failure in bootloader |
| PR #226 build with native Uint8Array | ❌ Data correct before write, corrupted after (this report) |
| PR #226 with `compress: false` | ❌ "Yet to handle Non Compressed writes" error |

## Conclusion

The corruption occurs **inside esptool-js** during the compressed write operation. PR #226's Uint8Array support does not fix this for ESP32-P4.

The same firmware files flash correctly with Python esptool:
```bash
python -m esptool --chip esp32p4 -p COM5 -b 460800 write_flash \
  --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

## Request

Could you investigate where the data corruption occurs in the compressed write path for ESP32-P4? We're happy to provide additional testing or logs if needed.

**Repository with test case:** https://github.com/fabkury/p3a

---

*Tested: December 20, 2025*

