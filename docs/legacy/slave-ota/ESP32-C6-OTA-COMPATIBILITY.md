# ESP32-C6 Slave OTA Compatibility Issue

> **Status:** ✅ Implemented & Verified (Dual-Version Support)  
> **Last Updated:** 2026-01-11  
> **Resolution:** Dual-version support — see [New Strategy](#new-strategy-dual-version-support) below

## Current Configuration

| Component | Version | File |
|-----------|---------|------|
| **esp_hosted (host)** | ≥2.9.3 | `main/idf_component.yml` |
| **network_adapter.bin (slave)** | 2.9.3 | `components/slave_ota/firmware/` |
| **Version constants** | 2.9.3 | `components/slave_ota/slave_ota.c` |
| **Locked version detection** | 2.7.0 | `components/slave_ota/slave_ota.c` |

> ✅ **Dual-Version Support Active:** The host library (2.9.3+) works with both legacy 2.7.0 slaves (with warnings) and newer slaves. Devices running 2.7.0 will skip OTA and continue operating. New factory devices (0.0.0) will be upgraded to 2.9.3.

## Verified OTA Paths (2026-01-11)

| From Version | To Version | Result | Notes |
|--------------|------------|--------|-------|
| 0.0.0 (factory) | 2.9.3 | ✅ **Success** | Fixed firmware size calculation |
| 2.7.0 | 2.9.3 | ❌ **Blocked** | Confirmed bug in 2.7.0 slave firmware |
| 2.9.3 | 2.9.3 | ✅ **Skip** | Already up to date |

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

| From Version | To Version | Result | Notes |
|--------------|------------|--------|-------|
| 0.0.0 (factory) | 2.7.0 | ✅ Success | Original implementation |
| 0.0.0 (factory) | 2.9.3 | ✅ Success | After size calculation fix (2026-01-11) |

### What Fails ❌

| From Version | To Version | Result | Error |
|--------------|------------|--------|-------|
| 2.7.0 | 2.7.4 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.8.4 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.8.5 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.9.1 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` |
| 2.7.0 | 2.9.3 | ❌ Fail | `ESP_ERR_OTA_VALIDATE_FAILED` (tested with official ota-test too) |

**Conclusion:** Once a device is running 2.7.0, it cannot be OTA updated to ANY other version. This was verified using both p3a and the official ESP-Hosted `host_performs_slave_ota` example.

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

### Previous Approach: Pin to esp_hosted 2.7.0 Exactly (Superseded)

The initial workaround was to pin everything to 2.7.0 exactly:

| Component | Version |
|-----------|---------|
| esp_hosted (host library) | 2.7.0 |
| network_adapter.bin (slave firmware) | 2.7.0 |

**Limitation:** This prevented new devices from receiving bug fixes and new features available in newer esp_hosted versions.

---

## New Strategy: Dual-Version Support

> **Decision Date:** 2026-01-10  
> **Verified:** 2026-01-11  
> **Rationale:** Espressif confirmed the 2.7.0 OTA bug is unrecoverable ([GitHub Issue #143](https://github.com/espressif/esp-hosted-mcu/issues/143#issuecomment-3732692355)). Rather than locking all devices to 2.7.0 forever, we support both versions simultaneously.

### Key Findings (2026-01-11)

1. **0.0.0 → 2.9.3 works** ✅ - After fixing a firmware size calculation bug in `slave_ota.c`, factory devices successfully upgrade.
2. **2.7.0 → anything fails** ❌ - This was verified using BOTH p3a AND the official `host_performs_slave_ota` example. The bug is in the 2.7.0 slave firmware's OTA validation logic.

### Key Insight

- **Devices with 2.7.0 slave:** Cannot be upgraded, but CAN still operate with a newer host library (backward compatible)
- **New factory devices (0.0.0):** Successfully upgrade to 2.9.3 with the fixed size calculation

### Target Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      ESP32-P4 (Host)                        │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  esp_hosted host library (2.9.1 or newer)             │  │
│  │  ├── Works with slave 2.7.0 (backward compatible)     │  │
│  │  └── Works with slave 2.9.1+ (native)                 │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Runtime OTA Decision Logic                           │  │
│  │  ├── If slave == 0.0.0  → Flash newest firmware ✅    │  │
│  │  ├── If slave == 2.7.0  → SKIP OTA, use as-is ⚠️      │  │
│  │  └── If slave >= target → Already up to date ✅       │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Expected Behavior by Device State

| Slave Version | OTA Action | Result |
|---------------|------------|--------|
| 0.0.0 (factory) | Flash newest | ✅ Gets latest firmware |
| 2.7.0 (locked) | Skip OTA | ⚠️ Works with warnings, no upgrade |
| ≥ target version | No action | ✅ Already up to date |

### Version Mismatch Warnings

When the host (2.9.1+) communicates with a 2.7.0 slave, esp_hosted will emit:
```
W (XXXX) transport: Version mismatch: Host [2.9.1] > Co-proc [2.7.0] ==> Upgrade co-proc to avoid RPC timeouts
```

**Decision:** Leave these warnings enabled as informational — they help operators identify legacy devices in the field.

### Implementation Plan

See **[DUAL-VERSION-SUPPORT-PLAN.md](DUAL-VERSION-SUPPORT-PLAN.md)** for detailed implementation steps.

### Previous Implementation Steps (for 2.7.0 pin, now superseded)

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

### Espressif Bug Report Status

Bug reported: [GitHub Issue #143](https://github.com/espressif/esp-hosted-mcu/issues/143)

**Espressif's response:** Confirmed as unrecoverable. Devices running 2.7.0 cannot be OTA-upgraded to any other version. This is a fundamental limitation of the 2.7.0 slave firmware's OTA validation logic.

### Technical Fix: Firmware Size Calculation

The initial 0.0.0 → 2.9.3 failure was caused by incorrect firmware size calculation. The fix was:

```c
// BEFORE (incorrect - 15 bytes short):
size_t padding = (16 - (fw_size % 16)) % 16;
fw_size += padding + 1;  // checksum
if (img_header.hash_appended == 1) {
    fw_size += 32;
}

// AFTER (correct):
fw_size = ((fw_size + 1) + 15) & ~15;  // Content + padding + checksum, aligned
if (img_header.hash_appended == 1) {
    fw_size += 32;
}
```

The ESP-IDF image format requires SHA256 hash to start at a 16-byte aligned address. The old formula incorrectly calculated padding, resulting in 15 fewer bytes being transferred. See [OTA-FAILURE-INVESTIGATION.md](OTA-FAILURE-INVESTIGATION.md) for detailed analysis.

### Long-Term Outlook

1. **Legacy 2.7.0 devices** - Will continue operating with basic WiFi functionality. May miss some newer features (DPP, WiFi Enterprise, etc.) but core functionality is preserved.

2. **New devices** - Will receive the latest firmware (2.9.3) and all new features.

3. **Potential future fix** - If Espressif releases a fix that somehow allows 2.7.0 upgrades (unlikely), the dual-version architecture can be updated to attempt OTA on those devices.

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

