# esptool-js Bug Report: ESP32-P4 Flash Settings Not Applied

**Repository:** https://github.com/espressif/esptool-js  
**esptool-js version:** 0.5.7  
**Affected chip:** ESP32-P4

---

## Summary

When using esptool-js to flash an ESP32-P4 device, the `flashMode`, `flashFreq`, and `flashSize` options passed to `writeFlash()` are ignored. The library outputs a warning indicating it cannot recognize the bootloader as a valid image file, and consequently does not apply the required flash settings. This results in a device that appears to flash successfully but fails to boot.

The same firmware files flash correctly using the Python esptool with explicit `--flash-mode dio --flash-freq 80m --flash-size 32MB` flags.

---

## Environment

- **esptool-js version:** 0.5.7 (via unpkg CDN)
- **Browser:** Google Chrome 131, Microsoft Edge 131
- **Operating System:** Windows 10/11
- **Target chip:** ESP32-P4 (revision v1.0)
- **Board:** Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
- **Flash:** 32MB, DIO mode required, 80MHz

---

## Steps to Reproduce

### 1. Prepare firmware files

Standard ESP-IDF build output for ESP32-P4:
- `bootloader.bin` (23,584 bytes) → address 0x2000
- `partition-table.bin` (3,072 bytes) → address 0x8000
- `ota_data_initial.bin` (8,192 bytes) → address 0x10000
- `app.bin` (~1.7 MB) → address 0x20000

### 2. Flash using esptool-js

```javascript
import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.5.7/bundle.js";

const device = await navigator.serial.requestPort({});
const transport = new Transport(device, true);
const esploader = new ESPLoader({ transport, baudrate: 460800 });
await esploader.main();

const flashOptions = {
  fileArray: [
    { data: bootloaderBinaryString, address: 0x2000 },
    { data: partitionTableBinaryString, address: 0x8000 },
    { data: otaDataBinaryString, address: 0x10000 },
    { data: appBinaryString, address: 0x20000 }
  ],
  flashSize: "32MB",
  flashMode: "dio",
  flashFreq: "80m",
  eraseAll: false,
  compress: true
};

await esploader.writeFlash(flashOptions);
```

### 3. Observe console output

```
esptool.js
Serial port WebSerial VendorID 0x1a86 ProductID 0x55d3
Connecting...
Detecting chip type...
ESP32-P4
Chip is ESP32-P4 (revision v1.0)
Features: High-Performance MCU
Crystal is 40MHz
MAC: 30:ed:a0:e2:08:69
Uploading stub...
Running stub...
Stub running...
Changing baudrate to 460800
Changed
Warning: Image file at 0x2000 doesn't look like an image file, so not changing any flash settings.
Compressed 23584 bytes to 14454...
Writing at 0x2000... (100%)
Wrote 23584 bytes (14454 compressed) at 0x2000 in 0.666 seconds.
Hash of data verified.
[...continues for all files...]
```

### 4. Result

- All files appear to write successfully
- Device **does not boot** after flashing
- Screen remains black, no serial output

---

## Expected Behavior

The `flashMode`, `flashFreq`, and `flashSize` options should be applied to configure the SPI flash regardless of whether the bootloader image header can be parsed.

When the warning "Image file at 0x2000 doesn't look like an image file" occurs, the library should **fall back to using the user-provided flash settings** instead of skipping flash configuration entirely.

---

## Working Workaround (Python esptool)

The same files flash correctly using Python esptool:

```bash
python -m esptool --chip esp32p4 -p COM5 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force \
  0x2000 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin \
  0x20000 app.bin
```

The device boots successfully after this command.

---

## Root Cause Analysis

### Bootloader Header Inspection

The ESP32-P4 bootloader has a valid ESP image header:

```
First 8 bytes: e9 03 02 5f da 9e f2 4f
              │  │  │  │
              │  │  │  └─ Flash size/freq: 0x5f = 32MB (5) + 80MHz (f)
              │  │  └──── Flash mode: 0x02 = DIO
              │  └─────── Segment count: 3
              └────────── Magic byte: 0xE9 ✓
```

The bootloader header already contains correct flash settings (DIO, 32MB, 80MHz). However, esptool-js reports it "doesn't look like an image file."

### Suspected Issue

The `updateImageFlashParams()` function in esptool-js may have ESP32-P4-specific header parsing that fails, possibly due to:

1. Different header structure for ESP32-P4 bootloader
2. Missing ESP32-P4 chip family in image validation logic
3. Stricter validation that rejects valid ESP32-P4 images

When image parsing fails, the code path that calls `flash_set_parameters()` with user-provided settings is not executed.

---

## Suggested Fix

When the image header cannot be parsed, the library should:

1. **Apply user-provided flash settings** if `flashMode`, `flashFreq`, or `flashSize` are explicitly passed
2. **Log a warning** that settings are being applied without image header validation
3. **Continue with the write operation** using the user's settings

Example pseudocode:

```javascript
if (!canParseImageHeader(data, address)) {
  if (userProvidedFlashSettings) {
    console.warn(`Using user-provided flash settings for address ${address}`);
    await this.flashSetParameters(userFlashMode, userFlashFreq, userFlashSize);
  } else {
    console.warn(`Cannot detect flash settings for ${address}, using defaults`);
  }
}
```

---

## Additional Information

### Verified Data Integrity

- SHA256 hashes of files match between working CLI flash and non-working JS flash
- File sizes are identical
- Binary string conversion verified correct

### Files Available

A complete reproduction case with firmware files is available at:
https://github.com/fabkury/p3a

### Impact

This issue prevents browser-based flashing for all ESP32-P4 projects, requiring users to install Python and use command-line tools instead of the more accessible web-based approach.

---

## Related

- ESP32-P4 is a relatively new chip (2024)
- esptool-js added ESP32-P4 target support in `src/targets/esp32p4.ts`
- Python esptool handles ESP32-P4 correctly with explicit flags

---

## Contact

- **Reporter:** @fabkury
- **Project:** p3a (Physical Pixel Art Player)
- **Repository:** https://github.com/fabkury/p3a

