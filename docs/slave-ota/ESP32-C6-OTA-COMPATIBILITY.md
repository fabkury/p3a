# ESP32-C6 Slave OTA Compatibility Issue

> **Status:** ✅ Resolved  
> **Last Updated:** 2026-01-08  
> **Resolution:** Pinned to esp_hosted 2.7.0 (see [Solution](#solution) below)

## Current Configuration

| Component | Version | File |
|-----------|---------|------|
| **esp_hosted (host)** | 2.7.0 | `main/idf_component.yml` |
| **network_adapter.bin (slave)** | 2.7.0 | `components/slave_ota/firmware/` |
| **Version constants** | 2.7.0 | `components/slave_ota/slave_ota.c` |

> ⚠️ **Note:** We are pinned to 2.7.0 exactly because OTA updates to ANY other version fail. See bug report: `docs/slave-ota/ESPRESSIF-BUG-REPORT.md`

## Problem Summary

The ESP32-C6 co-processor firmware cannot be upgraded from version **2.7.0** to **2.8.x** via OTA. The transfer completes 100%, but validation fails with `ESP_ERR_OTA_VALIDATE_FAILED`.

## Environment

| Component | Details |
|-----------|---------|
| **Host MCU** | ESP32-P4 |
| **Co-processor** | ESP32-C6 (onboard Waveshare ESP32-P4-WIFI6-Touch-LCD-4B) |
| **Transport** | SDIO |
| **ESP-IDF** | v5.5.1 |
| **esp_hosted host** | 2.8.4 (reports protocol 2.8.0) |

## Observed Behavior

### What Works ✅

| From Version | To Version | Result |
|--------------|------------|--------|
| 0.0.0 (factory) | 2.7.0 | ✅ Success |

### What Fails ❌

| From Version | To Version | Result | Error |
|--------------|------------|--------|-------|
| 2.7.0 | 2.7.4 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.8.4 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.8.5 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.9.1 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |

**Conclusion:** Once a device is running 2.7.0, it cannot be OTA updated to ANY other version.

## Error Details

### Console Output

```
I (9346) slave_ota: Current co-processor firmware: 2.7.0
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

### Key Observations

1. **Transfer completes 100%** - All 1,170,369 bytes are transferred successfully
2. **Validation fails on the slave side** - The ESP32-C6 (running 2.7.0) rejects the 2.8.x firmware
3. **RPC response code 5379** (0x1503) indicates validation failure
4. The issue is **not** related to:
   - Network connectivity
   - Firmware corruption during transfer
   - Partition size (2.8.x firmware is ~1.1MB, partition is 2MB)

## Root Cause Analysis

### Hypothesis

The **2.7.0 slave firmware's OTA validation logic** does not accept 2.8.x firmware. This is likely due to:

1. **Breaking changes in 2.8.0** - Version 2.8.0 introduced "Network Split (Shared IP)", a major architectural change
2. **Possible partition table changes** - The 2.8.x slave may expect a different partition layout
3. **Firmware format or metadata changes** - Something in the 2.8.x image format that 2.7.0's validator doesn't understand

### What Changed in 2.8.0

From the esp_hosted changelog:

> **2.8.0 - Network Split (Shared IP)**
> This major update allows the Host MCU and the ESP Slave to share a single IP address while running independent network stacks.

This was a significant architectural change that may have introduced incompatibilities.

## Constraints

- **Direct UART flashing is not an option** - The firmware must be deliverable to end users who only flash the ESP32-P4 via USB
- **The P4 must handle everything** - All C6 updates must happen via the SDIO OTA mechanism
- **Factory devices ship with 0.0.0 or 2.7.0** - Must support upgrading from these versions

## Solution

### Chosen Approach: Pin to esp_hosted 2.7.0 Exactly

Since OTA updates from 2.7.0 to ANY other version fail, we must match the version exactly:

| Component | Version |
|-----------|---------|
| esp_hosted (host library) | 2.7.0 |
| network_adapter.bin (slave firmware) | 2.7.0 |

This means:
- No OTA update is needed (versions already match)
- We cannot upgrade the C6 firmware via this mechanism
- A bug report has been filed with Espressif (see `ESPRESSIF-BUG-REPORT.md`)

### Implementation Steps

1. [x] Document the problem (this file)
2. [x] Pin `esp_hosted` to `==2.7.0` in `main/idf_component.yml`
3. [x] Build slave firmware from esp_hosted 2.7.0 sources
4. [x] Update `slave_ota.c` version constants to 2.7.0
5. [x] Copy new `network_adapter.bin` to `components/slave_ota/firmware/`
6. [x] Rebuild p3a project
7. [x] Create bug report for Espressif

## Files Modified

| File | Change |
|------|--------|
| `main/idf_component.yml` | Pin esp_hosted to ==2.7.0 |
| `components/slave_ota/slave_ota.c` | Update version constants to 2.7.0 |
| `components/slave_ota/firmware/network_adapter.bin` | Replace with 2.7.0 build |

## Upgrade Attempts

### 2.9.1 Attempt (2026-01-08)

Attempted to upgrade from 2.7.0 to 2.9.1. Result: **Failed** with `ESP_ERR_OTA_VALIDATE_FAILED`.

The upgrade was performed by:
1. Updating `main/idf_component.yml` to `==2.9.1`
2. Building the host project to download esp_hosted 2.9.1
3. Building the slave firmware from `managed_components/espressif__esp_hosted/slave/`
4. Flashing to ESP32-P4

On boot, the P4 detected version mismatch and attempted OTA update to the C6. Transfer completed 100%, but validation failed on the slave side - same error as all previous upgrade attempts.

## Future Considerations

### Upgrading esp_hosted is Not Currently Possible

All tested versions (2.7.4, 2.8.4, 2.8.5, 2.9.1) fail with the same `ESP_ERR_OTA_VALIDATE_FAILED` error when attempting to OTA update from 2.7.0. This appears to be a fundamental limitation of the slave's OTA validation logic.

Options if newer features are needed:
1. **Report bug to Espressif** - This appears to be a legitimate compatibility issue
   - GitHub: https://github.com/espressif/esp-hosted-mcu/issues
2. **Direct UART flashing** - Not viable for end-user devices (requires opening enclosure and connecting to C6 debug port)

### Related esp_hosted Changelog Entries

```
## 2.7.4 - Bug Fixes
- fixed co-processor to properly allow wifi init and deinit
- fixed registration of event handlers in co-processor

## 2.7.3 - Bug Fixes
- fixed RPC Response for OTA commands to return errors in responses correctly
- fixed double free bug in host OTA example

## 2.7.2 - Bug Fixes
- Stable workaround for ota writes to slave

## 2.7.1
- Add support for more PCBs (ESP32-P4 Core Board with C5/C6)

## 2.7.0
- Restructured the ESP-Hosted-MCU commits
```

## References

- [esp_hosted component](https://components.espressif.com/components/espressif/esp_hosted)
- [esp-hosted-mcu GitHub](https://github.com/espressif/esp-hosted-mcu)
- [Host Performs Slave OTA Example](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/host_performs_slave_ota)

