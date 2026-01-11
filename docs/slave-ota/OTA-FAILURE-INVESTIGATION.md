# ESP32-C6 Slave OTA Failure Investigation

**Date:** 2026-01-11  
**Status:** ✅ Resolved  
**Issue:** p3a fails to OTA update factory devices (0.0.0 → 2.9.3) while `ota-test` succeeds

## Executive Summary

The `ota-test` example project successfully upgraded a factory-fresh ESP32-C6 (v0.0.0) to v2.9.3, but p3a initially failed with `ESP_ERR_OTA_VALIDATE_FAILED` on the same operation. This investigation identified the root cause as an **incorrect firmware size calculation** that was 15 bytes short.

### Resolution

1. **0.0.0 → 2.9.3: FIXED** ✅ - The size calculation was corrected, and factory devices now successfully upgrade
2. **2.7.0 → 2.9.3: CONFIRMED BROKEN** ❌ - Testing revealed that 2.7.0 devices cannot be OTA-upgraded even with the correct size calculation. This was verified with both p3a AND the official `ota-test` example.

### Final Implementation

- Factory devices (0.0.0) are upgraded to 2.9.3 on first boot
- Devices already running 2.7.0 are detected and skipped (they continue operating with 2.7.0)
- Devices already running 2.9.3 are detected as up-to-date

## Key Observations

### Successful OTA (ota-test)
```
I (2475) ota_littlefs: File stat successful for network_adapter.bin, size: 1167008
I (2552) ota_littlefs: Total image size: 1167009 bytes
I (21259) ota_littlefs: LittleFS OTA completed successfully
```

### Failed OTA (p3a)
```
I (6726) slave_ota: Starting co-processor OTA update (1170353 bytes)...
I (33886) slave_ota: OTA progress: 100% (1170353 bytes)
I (33886) slave_ota: Firmware transfer complete (1170353 bytes), finalizing...
W (34054) rpc_rsp: Hosted RPC_Resp [0x212], uid [846], resp code [5379]
E (34090) RPC_WRAP: OTA procedure failed
E (34091) slave_ota: esp_hosted_slave_ota_end failed: ESP_ERR_OTA_VALIDATE_FAILED
```

## Root Causes Identified

### 1. Different Firmware Transfer Mechanisms

| Aspect | ota-test (LittleFS) | p3a (Partition) |
|--------|---------------------|-----------------|
| **Data source** | File in LittleFS filesystem | Raw partition data |
| **Size determination** | `fread()` until EOF | Calculated from image header |
| **Transfer method** | Reads entire file content | Reads exactly calculated bytes |

**Critical difference:** The LittleFS method uses `fread()` in a loop until EOF, transferring the **entire file content** regardless of what the image header indicates:

```c
// ota_littlefs.c (ota-test) - Lines 410-419
while ((bytes_read = fread(chunk, 1, CHUNK_SIZE, firmware_file)) > 0) {
    ret = esp_hosted_slave_ota_write(chunk, bytes_read);
    // ...
}
```

The partition method (both p3a and ota_partition.c in ota-test) calculates the size:

```c
// slave_ota.c (p3a) - Lines 142-167
size_t fw_size = sizeof(esp_image_header_t);
// ... parse segments ...
fw_size += padding;
fw_size += 1;  // checksum
if (img_header.hash_appended == 1) {
    fw_size += 32;  // SHA256
}
```

### 2. Firmware File Size Discrepancy

| Source | Size (bytes) |
|--------|-------------|
| Actual file on disk | 1,170,368 |
| ota-test file (from logs) | 1,167,008 |
| p3a calculated size | 1,170,353 |
| Difference (file vs calculated) | **15 bytes** |

**Key insight:** The ota-test logs show a file size of **1,167,008 bytes**, but the current firmware file is **1,170,368 bytes**. These are **different files**!

The successful OTA used a different (smaller) firmware binary than what p3a is attempting to use.

### 3. Size Calculation vs Actual File Size

The 15-byte discrepancy between the file size (1,170,368) and calculated size (1,170,353) suggests:
- The firmware binary may contain trailing padding beyond the image structure
- The size calculation might be missing some metadata
- Flash alignment padding might be included in the .bin file but not in the calculation

## Technical Analysis

### ESP-IDF Firmware Binary Structure

```
┌─────────────────────────────┐
│ esp_image_header_t (24B)    │
├─────────────────────────────┤
│ Segment 0 header (8B)       │
│ Segment 0 data              │
├─────────────────────────────┤
│ Segment 1 header (8B)       │
│ Segment 1 data              │
├─────────────────────────────┤
│ ... more segments ...       │
├─────────────────────────────┤
│ Padding (16-byte align)     │
├─────────────────────────────┤
│ Checksum (1B)               │
├─────────────────────────────┤
│ SHA256 hash (32B, optional) │
├─────────────────────────────┤
│ ??? Trailing data ???       │  ← May be present in .bin file
└─────────────────────────────┘
```

The LittleFS method works because it sends **everything** in the file, including any trailing data. The partition method fails because it only sends the **calculated** size, potentially missing required trailing bytes.

### Why ota-test Worked

1. Used the LittleFS OTA method (`CONFIG_OTA_METHOD_LITTLEFS=y`)
2. The `fread()` loop transferred the entire file content
3. The slave received all bytes it expected for validation

### Why p3a Failed

1. Uses partition-based reading with calculated size
2. The calculated size (1,170,353) is 15 bytes short of the actual file (1,170,368)
3. The slave's OTA validation fails because it doesn't receive the complete image

## Evidence: ota_partition.c Uses Same Approach as p3a

The official ESP-Hosted example's partition method (`ota_partition.c`) uses the **exact same** size calculation approach as p3a:

```c
// ota_partition.c - Lines 133-188
offset = sizeof(image_header);
total_size = sizeof(image_header);

for (int i = 0; i < image_header.segment_count; i++) {
    // ... read segment ...
    total_size += sizeof(segment_header) + segment_header.data_len;
    offset += sizeof(segment_header) + segment_header.data_len;
}

size_t padding = (16 - (total_size % 16)) % 16;
total_size += padding;
total_size += 1;  // checksum

if (has_hash) {
    total_size += 32;  // SHA256
}
```

This suggests that if p3a had used the **LittleFS method**, it would have succeeded.

## Proposed Solutions

### Solution A: Use File-Based Transfer (Recommended)

Modify p3a's `slave_ota.c` to transfer the **entire firmware content** rather than calculating the size:

1. Read the firmware file size from the partition metadata (if available)
2. Or, scan for the end of valid data in the partition
3. Or, store the firmware size alongside the binary during flashing

### Solution B: Match ota-test's LittleFS Approach

Implement LittleFS-based OTA in p3a:
1. Store `network_adapter.bin` in LittleFS instead of raw partition
2. Use `fread()` until EOF to transfer all bytes
3. This matches the proven working approach

### Solution C: Fix Size Calculation

Investigate why there's a 15-byte discrepancy:
1. Check if the binary has additional trailing data
2. Verify the padding calculation is correct
3. Consider reading the entire partition content up to the first 0xFF run

### Solution D: Store Size Metadata

During build/flash:
1. Calculate the exact firmware file size
2. Store it in a known location (NVS, partition header, etc.)
3. Read this stored size during OTA instead of calculating

## Recommended Approach

**Solution C (Fix Size Calculation)** is the most pragmatic:

```c
// Modified approach: send the entire file, not calculated size
// Find actual content size by scanning for trailing 0xFF bytes
size_t actual_fw_size = partition->size;
uint8_t tail_check[256];

// Scan backwards from end of partition to find actual content end
for (size_t pos = partition->size; pos > 0; pos -= sizeof(tail_check)) {
    size_t check_pos = (pos > sizeof(tail_check)) ? pos - sizeof(tail_check) : 0;
    esp_partition_read(partition, check_pos, tail_check, sizeof(tail_check));
    
    // Find last non-0xFF byte
    for (int i = sizeof(tail_check) - 1; i >= 0; i--) {
        if (tail_check[i] != 0xFF) {
            actual_fw_size = check_pos + i + 1;
            goto found_end;
        }
    }
}
found_end:
```

Or simply use the **file size stored during flash** if the build system can embed this information.

## Verified Size Calculation Analysis

A direct analysis of the firmware binary confirms the 15-byte discrepancy:

```
Total file size: 1,170,368 bytes
Magic: 0xe9
Segment count: 5
Hash appended: 1

Segment 0: load_addr=0x420d0020, data_len=226,840
Segment 1: load_addr=0x40800000, data_len=35,288
Segment 2: load_addr=0x42000020, data_len=794,784
Segment 3: load_addr=0x408089d8, data_len=98,816
Segment 4: load_addr=0x40820be0, data_len=14,528

Header (24) + Segments (1,170,296) = 1,170,320
After 16-byte padding (0 bytes): 1,170,320
After checksum (1 byte): 1,170,321
After SHA256 (32 bytes): 1,170,353

Calculated size: 1,170,353 bytes
Actual file size: 1,170,368 bytes
Difference: 15 bytes
```

### Trailing Bytes Analysis

The 15 "missing" bytes are NOT empty padding:
```
Trailing 15 bytes hex: 9d354d7cfc41494055a1af87311371
All zeros: False
All 0xFF: False
```

These bytes appear to be part of the image content - likely additional padding inserted by esptool for flash alignment that is NOT accounted for in the standard image header calculation.

### Root Cause Confirmed

The standard ESP-IDF image size calculation formula:
```
size = header + Σ(segment_headers + segment_data) + padding_to_16 + checksum + SHA256
```

**Does NOT account for** the additional trailing bytes that esptool adds to the binary. The ESP-Hosted slave OTA validation expects the **complete** binary file, including these trailing bytes.

## Conclusion

The OTA failure is caused by **p3a sending 15 bytes fewer than the slave expects**. The size calculation formula used by p3a (and the official `ota_partition.c` example) does not account for trailing bytes added by esptool.

The ota-test succeeded because:
1. The LittleFS method uses `fread()` until EOF
2. This transfers ALL bytes in the file, including the trailing 15 bytes
3. The slave receives the complete, valid firmware image

The fix should ensure p3a transfers the **exact file content** rather than relying on size calculation.

## Recommended Fix

### Understanding the Correct Padding Formula

Examining the actual binary layout reveals:
- Content ends at offset **1,170,320**
- **15 bytes** of zero padding (0x00) follow
- Checksum byte `0xa9` at offset **1,170,335**
- SHA256 hash (32 bytes) starts at offset **1,170,336** (which is 16-byte aligned!)
- File ends at **1,170,368**

The ESP-IDF image format requires the **SHA256 hash to start at a 16-byte aligned address**. The current calculation incorrectly assumes padding aligns the content itself, not the SHA256 start position.

### Current (Incorrect) Code

```c
// slave_ota.c lines 157-166
size_t padding = (16 - (fw_size % 16)) % 16;  // WRONG: aligns content, not SHA256
fw_size += padding;
fw_size += 1;  // checksum
if (img_header.hash_appended == 1) {
    fw_size += 32;
}
```

This produces: 1,170,320 + 0 + 1 + 32 = **1,170,353** (15 bytes short!)

### Correct Formula

```c
// After summing all segments, fw_size = content size
// SHA256 must start at 16-byte aligned address
// Checksum byte is at (aligned_address - 1)
// Padding fills the gap between content and checksum

// Calculate where SHA256 should start (next 16-byte boundary after content + checksum)
fw_size = ((fw_size + 1) + 15) & ~15;  // Align (content + checksum) to 16 bytes

// Add SHA256 if appended
if (img_header.hash_appended == 1) {
    fw_size += 32;
}
```

This produces: ((1,170,320 + 1) + 15) & ~15 = **1,170,336**, then + 32 = **1,170,368** ✓

### Alternative Simpler Fix

If you don't want to change the padding logic, simply read 16 extra bytes:

```c
// After all calculations, round up to next 16-byte boundary
fw_size = (fw_size + 15) & ~15;
```

This is safe because the extra bytes will either be:
- Part of the valid image (as we discovered)
- Harmless 0xFF padding from the flash

## Files Referenced

- `components/slave_ota/slave_ota.c` - p3a's OTA implementation
- `ota-test/host_performs_slave_ota/components/ota_littlefs/ota_littlefs.c` - Working LittleFS method
- `ota-test/host_performs_slave_ota/components/ota_partition/ota_partition.c` - Official partition method

---

## Final Outcome (2026-01-11)

### Fix Applied

The firmware size calculation in `slave_ota.c` was corrected:

```c
// BEFORE (incorrect - 15 bytes short):
size_t padding = (16 - (fw_size % 16)) % 16;
fw_size += padding;
fw_size += 1;  // checksum
if (img_header.hash_appended == 1) {
    fw_size += 32;
}

// AFTER (correct):
fw_size = ((fw_size + 1) + 15) & ~15;  // Content + padding + checksum, aligned
if (img_header.hash_appended == 1) {
    fw_size += 32;
}
```

### Test Results After Fix

| Source Version | Target Version | Result | Notes |
|----------------|----------------|--------|-------|
| 0.0.0 (factory) | 2.9.3 | ✅ **Success** | Fix resolved the issue |
| 2.7.0 | 2.9.3 | ❌ **Fail** | Confirmed broken even with ota-test |

### 2.7.0 Bug Verification

The 2.7.0 → 2.9.3 failure was verified using **both** p3a and the official `ota-test` example:

```
I (2585) ota_littlefs: Starting OTA from LittleFS: /littlefs/network_adapter.bin
W (14611) rpc_rsp: Hosted RPC_Resp [0x212], uid [785], resp code [5379]
E (14611) RPC_WRAP: OTA procedure failed
E (14611) ota_littlefs: Failed to end OTA: ESP_ERR_OTA_VALIDATE_FAILED
```

This confirms that the 2.7.0 OTA bug is **in the slave firmware itself**, not in any host-side code. The official example also fails, proving this is an upstream Espressif issue.

### Implementation

p3a now implements **dual-version support**:

1. **0.0.0 devices** → Upgraded to 2.9.3 ✅
2. **2.7.0 devices** → Skipped (continue with 2.7.0) ⚠️
3. **2.9.3 devices** → Already up to date ✅

See `components/slave_ota/slave_ota.c` for the implementation.
