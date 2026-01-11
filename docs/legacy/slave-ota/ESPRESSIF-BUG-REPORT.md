# ESP-Hosted OTA Bug Report

**Repository:** https://github.com/espressif/esp-hosted-mcu  
**Component:** esp_hosted (ESP Component Registry)  
**Severity:** Critical - Blocks firmware updates  
**Status:** Confirmed by Espressif - 2.7.0 is unrecoverable  
**Last Updated:** 2026-01-11

---

## Summary

ESP-Hosted slave firmware running version **2.7.0** rejects OTA updates to **any other version** (including 2.7.4, 2.8.x, and 2.9.x) with `ESP_ERR_OTA_VALIDATE_FAILED`. The firmware transfer completes 100% successfully, but validation fails on the slave side.

### Update (2026-01-11)

This bug was verified using **both** our custom implementation AND the official `host_performs_slave_ota` example from ESP-Hosted. Both fail identically when attempting to upgrade from 2.7.0.

**Note:** OTA updates FROM 0.0.0 (factory) TO 2.9.3 work correctly after fixing a separate firmware size calculation issue in the host code. The 2.7.0 issue is specifically in the 2.7.0 slave firmware's OTA validation logic.

---

## Environment

| Component | Value |
|-----------|-------|
| **Host MCU** | ESP32-P4 |
| **Co-processor (Slave)** | ESP32-C6 |
| **Transport** | SDIO (4-bit, 40MHz) |
| **Board** | Waveshare ESP32-P4-WIFI6-Touch-LCD-4B |
| **ESP-IDF Version** | v5.5.1 |
| **Host OS** | Windows 10 |

### GPIO Configuration (from logs)

```
GPIOs: CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]
```

---

## Steps to Reproduce

### Initial State
- ESP32-C6 co-processor running esp_hosted slave firmware **v2.7.0**
- (Note: Device was successfully updated from factory 0.0.0 → 2.7.0 via the same OTA mechanism)

### Reproduction Steps

1. Build esp_hosted host library at version 2.7.4 (or 2.8.x)
2. Build slave firmware at matching version:
   ```bash
   idf.py create-project-from-example "espressif/esp_hosted==2.7.4:slave"
   cd slave
   idf.py set-target esp32c6
   # Verify SDIO is configured (CONFIG_ESP_SDIO_HOST_INTERFACE=y)
   idf.py build
   ```
3. Embed the `network_adapter.bin` in a data partition on the ESP32-P4
4. Use `esp_hosted_slave_ota_*` APIs to transfer firmware to the ESP32-C6:
   ```c
   esp_hosted_slave_ota_begin();
   // Transfer firmware in chunks via esp_hosted_slave_ota_write()
   esp_hosted_slave_ota_end();  // <-- FAILS HERE
   ```

### Expected Result
OTA update succeeds; ESP32-C6 reboots with new firmware.

### Actual Result
`esp_hosted_slave_ota_end()` returns `ESP_ERR_OTA_VALIDATE_FAILED`.

---

## Tested Version Combinations

| From (Slave) | To (New Firmware) | Result | Test Method |
|--------------|-------------------|--------|-------------|
| 0.0.0 (factory) | 2.7.0 | ✅ **Success** | p3a |
| 0.0.0 (factory) | 2.9.3 | ✅ **Success** | p3a (after size calc fix) |
| 0.0.0 (factory) | 2.9.3 | ✅ **Success** | Official ota-test example |
| 2.7.0 | 2.7.4 | ❌ **Fail** | p3a |
| 2.7.0 | 2.8.4 | ❌ **Fail** | p3a |
| 2.7.0 | 2.8.5 | ❌ **Fail** | p3a |
| 2.7.0 | 2.9.3 | ❌ **Fail** | p3a |
| 2.7.0 | 2.9.3 | ❌ **Fail** | Official ota-test example (LittleFS) |

**Key observation:** The 2.7.0 → any version failure was verified using BOTH our custom implementation AND the official ESP-Hosted example. This confirms the bug is in the 2.7.0 slave firmware, not in host-side code.

---

## Console Logs

### Attempt: 2.7.0 → 2.7.4

```
I (4092) H_SDIO_DRV: SDIO Host operating in STREAMING MODE
I (4092) H_SDIO_DRV: Open data path at slave
I (4092) H_SDIO_DRV: Starting SDIO process rx task
I (4119) H_SDIO_DRV: Received ESP_PRIV_IF type message
I (4120) transport: Received INIT event from ESP32 peripheral
I (4120) transport: EVENT: 12
I (4121) transport: Identified slave [esp32c6]
I (4126) transport: EVENT: 11
I (4128) transport: capabilities: 0xd
I (4132) transport: Features supported are:
I (4136) transport:      * WLAN
I (4138) transport:        - HCI over SDIO
I (4142) transport:        - BLE only
I (4145) transport: EVENT: 13
W (4148) transport: Version mismatch: Host [2.7.0] > Co-proc [2.7.0] ==> Upgrade co-proc to avoid RPC timeouts
I (4157) transport: ESP board type is : 13
...
I (10078) slave_ota: Checking ESP32-C6 co-processor firmware...
I (10108) slave_ota: Current co-processor firmware: 2.7.0
I (10113) slave_ota: Embedded slave firmware: 2.7.4
W (10117) slave_ota: Co-processor firmware update required!
I (10123) slave_ota: Found slave firmware partition: offset=0x1420000, size=0x200000
I (10130) slave_ota: Slave firmware in partition: network_adapter v2.7.4
I (10137) slave_ota: Starting co-processor OTA update (1167057 bytes)...
I (11761) slave_ota: OTA progress: 10% (117600 bytes)
I (12954) slave_ota: OTA progress: 20% (233800 bytes)
I (14147) slave_ota: OTA progress: 30% (351400 bytes)
I (15340) slave_ota: OTA progress: 40% (467600 bytes)
I (16520) slave_ota: OTA progress: 50% (583800 bytes)
I (17776) slave_ota: OTA progress: 60% (701400 bytes)
I (18968) slave_ota: OTA progress: 70% (817600 bytes)
I (20152) slave_ota: OTA progress: 80% (933800 bytes)
I (21346) slave_ota: OTA progress: 90% (1051400 bytes)
I (22520) slave_ota: OTA progress: 100% (1167057 bytes)
I (22520) slave_ota: Firmware transfer complete (1167057 bytes), finalizing...
W (22538) rpc_rsp: Hosted RPC_Resp [0x212], uid [846], resp code [5379]
E (22551) RPC_WRAP: OTA procedure failed
E (22551) slave_ota: esp_hosted_slave_ota_end failed: ESP_ERR_OTA_VALIDATE_FAILED
```

### Attempt: 2.7.0 → 2.8.5 (same result)

```
I (9326) slave_ota: Current co-processor firmware: 2.7.0
I (9351) slave_ota: Embedded slave firmware: 2.8.5
W (9356) slave_ota: Co-processor firmware update required!
I (9361) slave_ota: Found slave firmware partition: offset=0x1420000, size=0x200000
I (9368) slave_ota: Slave firmware in partition: network_adapter v2.8.5
I (9375) slave_ota: Starting co-processor OTA update (1170369 bytes)...
I (11235) slave_ota: OTA progress: 10% (117600 bytes)
...
I (22021) slave_ota: OTA progress: 100% (1170369 bytes)
I (22021) slave_ota: Firmware transfer complete (1170369 bytes), finalizing...
W (22039) rpc_rsp: Hosted RPC_Resp [0x212], uid [848], resp code [5379]
E (22039) RPC_WRAP: OTA procedure failed
E (22039) slave_ota: esp_hosted_slave_ota_end failed: ESP_ERR_OTA_VALIDATE_FAILED
```

---

## OTA Implementation Details

We use the standard esp_hosted OTA APIs as documented:

```c
// 1. Begin OTA
esp_err_t err = esp_hosted_slave_ota_begin();

// 2. Transfer firmware in chunks
while (offset < firmware_size) {
    esp_partition_read(partition, offset, buffer, chunk_size);
    esp_hosted_slave_ota_write(buffer, chunk_size);
    offset += chunk_size;
}

// 3. End OTA (validate) - THIS FAILS
err = esp_hosted_slave_ota_end();  // Returns ESP_ERR_OTA_VALIDATE_FAILED

// 4. Activate (never reached)
esp_hosted_slave_ota_activate();
```

### Firmware Size Calculation

We calculate firmware size by parsing the ESP image header (same method as `host_performs_slave_ota` example):

```c
esp_image_header_t img_header;
esp_partition_read(partition, 0, &img_header, sizeof(img_header));

size_t fw_size = sizeof(esp_image_header_t);
for (int i = 0; i < img_header.segment_count; i++) {
    esp_image_segment_header_t seg_header;
    esp_partition_read(partition, offset, &seg_header, sizeof(seg_header));
    fw_size += sizeof(seg_header) + seg_header.data_len;
    offset += sizeof(seg_header) + seg_header.data_len;
}

// Add padding, checksum, and SHA256 hash
size_t padding = (16 - (fw_size % 16)) % 16;
fw_size += padding + 1;  // checksum byte
if (img_header.hash_appended == 1) {
    fw_size += 32;  // SHA256
}
```

---

## Analysis

### What We Know

1. **OTA mechanism works** - Successfully updated 0.0.0 → 2.7.0
2. **Transfer is complete** - 100% of bytes are sent successfully
3. **Validation fails on slave** - The ESP32-C6 running 2.7.0 rejects the firmware
4. **RPC response code 5379** (0x1503) indicates validation failure
5. **Not version-specific** - Fails for both 2.7.x and 2.8.x target versions

### Possible Causes

1. **Bug in 2.7.0's OTA validation logic** - The 2.7.0 slave firmware may have a bug that causes it to reject valid firmware images
2. **Partition table mismatch** - The 2.7.0 slave may expect a specific partition layout
3. **Firmware format change** - Something in how the firmware is structured changed after 2.7.0
4. **Anti-rollback or secure boot** - Unlikely since we're upgrading, not downgrading

---

## Impact

This bug **completely blocks OTA updates** for devices running esp_hosted 2.7.0. For products deployed in the field, this means:

- Cannot push bug fixes to the co-processor
- Cannot upgrade to get new features
- Stuck on 2.7.0 forever unless physically accessing the device for UART flashing

---

## Workaround

### Current Solution: Dual-Version Support

We've implemented dual-version support in p3a:

1. **Factory devices (0.0.0)** → Automatically upgraded to 2.9.3 ✅
2. **Legacy devices (2.7.0)** → Detected and skipped; continue operating with 2.7.0 ⚠️
3. **Up-to-date devices (2.9.3)** → No action needed ✅

The host library (2.9.3) is backward compatible with 2.7.0 slaves, so legacy devices continue working normally (with version mismatch warnings in logs).

### Previous Workaround (Superseded)

Previously pinning to esp_hosted 2.7.0 exactly for both host and slave.

---

## Requested Information

1. Is this a known issue?
2. What changed in 2.7.0's OTA validation compared to earlier versions?
3. Is there a specific build configuration needed for OTA-compatible firmware?
4. Are there any debug logs we can enable on the slave side to understand why validation fails?

---

## Attachments

- Full console logs available upon request
- Build configuration (sdkconfig) available upon request
- Custom OTA implementation code available upon request

---

## Contact

Please respond to this issue on GitHub or contact the reporter directly.

