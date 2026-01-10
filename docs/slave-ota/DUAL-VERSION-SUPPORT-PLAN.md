# Dual-Version esp_hosted Support Implementation Plan

> **Created:** 2026-01-10
> **Status:** ✅ Implemented
> **Related:** [ESP32-C6-OTA-COMPATIBILITY.md](ESP32-C6-OTA-COMPATIBILITY.md)

## Executive Summary

This document outlines the plan to support both esp_hosted 2.7.0 and newer versions (2.9.1+) simultaneously in the p3a codebase. This allows new devices to receive the latest firmware while legacy devices locked on 2.7.0 continue operating.

---

## Background

### The Problem

- ESP32-C6 slave devices running version **2.7.0** cannot be OTA-upgraded to any newer version
- Espressif has confirmed this is **unrecoverable** ([GitHub Issue #143](https://github.com/espressif/esp-hosted-mcu/issues/143#issuecomment-3732692355))
- New factory devices ship with version **0.0.0** and CAN be upgraded normally

### The Decision

Rather than locking all devices to 2.7.0 forever, p3a will:

1. **Upgrade the host library** to 2.9.1 or newer
2. **Embed the newest slave firmware** for new devices
3. **Detect and skip OTA** for devices already running 2.7.0
4. **Accept version mismatch warnings** as informational

---

## Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────────┐
│                       ESP32-P4 Host                             │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │            esp_hosted Library (2.9.1+)                   │   │
│  │                                                          │   │
│  │  • Backward compatible with 2.7.0 slave                  │   │
│  │  • Native support for 2.9.1+ slave                       │   │
│  │  • RPC protocol handles version differences gracefully   │   │
│  └──────────────────────────────────────────────────────────┘   │
│                              │                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │              slave_ota.c (Modified)                      │   │
│  │                                                          │   │
│  │  slave_ota_check_and_update():                           │   │
│  │    1. Detect slave version                               │   │
│  │    2. If version == 2.7.0:                               │   │
│  │         → Log warning, SKIP OTA, return success          │   │
│  │    3. If version == 0.0.0 or needs upgrade:              │   │
│  │         → Proceed with OTA as normal                     │   │
│  │    4. If version >= target:                              │   │
│  │         → Already up to date, skip                       │   │
│  └──────────────────────────────────────────────────────────┘   │
│                              │                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │         Embedded Slave Firmware Partition                │   │
│  │                                                          │   │
│  │         network_adapter.bin (2.9.1+)                     │   │
│  │         └── Only the newest version embedded             │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                               │
                          SDIO Transport
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                       ESP32-C6 Slave                            │
│                                                                 │
│  Running one of:                                                │
│    • 0.0.0 (factory) → Will be upgraded to 2.9.1+              │
│    • 2.7.0 (locked)  → Cannot upgrade, works with warnings     │
│    • 2.9.1+ (new)    → Already up to date                      │
└─────────────────────────────────────────────────────────────────┘
```

### Runtime Decision Flow

```
                    ┌─────────────────┐
                    │  System Boot    │
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │  Initialize     │
                    │  esp_hosted     │
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │  Detect slave   │
                    │    version      │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
              ▼              ▼              ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │ v0.0.0   │   │ v2.7.0   │   │ v2.9.1+  │
        │ (factory)│   │ (locked) │   │ (current)│
        └────┬─────┘   └────┬─────┘   └────┬─────┘
             │              │              │
             ▼              ▼              ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │ Flash    │   │ SKIP OTA │   │ No action│
        │ 2.9.1+   │   │ Log warn │   │ needed   │
        └────┬─────┘   └────┬─────┘   └────┬─────┘
             │              │              │
             └──────────────┼──────────────┘
                            │
                            ▼
                    ┌─────────────────┐
                    │  Continue with  │
                    │  WiFi init      │
                    └─────────────────┘
```

---

## Implementation Steps

### Phase 1: Update Dependencies

1. **Update `main/idf_component.yml`**
   - Change `espressif/esp_hosted: "==2.7.0"` to `espressif/esp_hosted: ">=2.9.1"`
   - Run `idf.py reconfigure` to download the new component

2. **Delete `dependencies.lock`** (optional)
   - Forces fresh dependency resolution
   - Ensures newest compatible version is pulled

### Phase 2: Build New Slave Firmware

1. **Navigate to slave project**
   - Location: `managed_components/espressif__esp_hosted/slave/`

2. **Configure for ESP32-C6 + SDIO**
   - `idf.py set-target esp32c6`
   - Ensure SDIO interface is enabled in menuconfig

3. **Build the slave firmware**
   - `idf.py build`
   - Output: `build/network_adapter.bin`

4. **Copy to p3a project**
   - Destination: `components/slave_ota/firmware/network_adapter.bin`

### Phase 3: Modify slave_ota.c

1. **Update version constants**
   - Change `SLAVE_FW_VERSION_*` to match the new firmware version

2. **Add 2.7.0 detection and skip logic**
   - In `slave_ota_check_and_update()`, add special handling for version 2.7.0
   - Log a clear warning explaining why OTA is skipped
   - Return `ESP_OK` to allow normal operation to continue

3. **Update version comparison logic**
   - Ensure proper handling of all version scenarios

### Phase 4: Testing

1. **Test with factory device (0.0.0)**
   - Verify OTA proceeds and completes successfully
   - Verify device reboots with new firmware
   - Verify WiFi functionality after upgrade

2. **Test with legacy device (2.7.0)**
   - Verify OTA is skipped with appropriate log message
   - Verify no OTA attempt is made
   - Verify WiFi functionality works despite version mismatch
   - Note: Version mismatch warning is expected and acceptable

3. **Test with already-upgraded device (2.9.1+)**
   - Verify no OTA action taken (already up to date)
   - Verify normal operation

### Phase 5: Documentation

1. **Update this plan** with results and any deviations
2. **Update ESP32-C6-OTA-COMPATIBILITY.md** status to "Implemented"
3. **Consider updating README.md** if user-facing impact

---

## Expected Log Output

### Scenario A: Factory Device (0.0.0)

```
I (XXXX) slave_ota: Checking ESP32-C6 co-processor firmware...
I (XXXX) slave_ota: Current co-processor firmware: 0.0.0
I (XXXX) slave_ota: Embedded slave firmware: 2.9.1
W (XXXX) slave_ota: Co-processor firmware update required!
I (XXXX) slave_ota: Starting co-processor OTA update (XXXXXX bytes)...
I (XXXX) slave_ota: OTA progress: 10% ...
...
I (XXXX) slave_ota: Co-processor firmware updated successfully!
```

### Scenario B: Legacy Device (2.7.0)

```
I (XXXX) slave_ota: Checking ESP32-C6 co-processor firmware...
I (XXXX) slave_ota: Current co-processor firmware: 2.7.0
I (XXXX) slave_ota: Embedded slave firmware: 2.9.1
W (XXXX) slave_ota: Detected esp_hosted 2.7.0 - OTA not possible (known issue)
W (XXXX) slave_ota: See: https://github.com/espressif/esp-hosted-mcu/issues/143
W (XXXX) slave_ota: Device will continue operating with 2.7.0 slave firmware
I (XXXX) slave_ota: Proceeding without co-processor update
```

Plus the expected esp_hosted transport warning:
```
W (XXXX) transport: Version mismatch: Host [2.9.1] > Co-proc [2.7.0] ==> Upgrade co-proc to avoid RPC timeouts
```

### Scenario C: Up-to-Date Device (2.9.1)

```
I (XXXX) slave_ota: Checking ESP32-C6 co-processor firmware...
I (XXXX) slave_ota: Current co-processor firmware: 2.9.1
I (XXXX) slave_ota: Embedded slave firmware: 2.9.1
I (XXXX) slave_ota: Co-processor firmware is up to date
```

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| RPC timeouts with 2.7.0 slave | Low-Medium | Low | esp_hosted designed for backward compat; monitor in field |
| Missing features on 2.7.0 | Certain | Low | Core WiFi works; document limitations |
| Build issues with new version | Low | Medium | Test thoroughly before release |
| Unexpected API changes | Low | Medium | Review esp_hosted changelog before upgrading |

---

## Files to Modify

| File | Change |
|------|--------|
| `main/idf_component.yml` | Update esp_hosted version constraint |
| `components/slave_ota/slave_ota.c` | Add 2.7.0 detection, update version constants |
| `components/slave_ota/firmware/network_adapter.bin` | Replace with 2.9.1+ build |
| `docs/slave-ota/ESP32-C6-OTA-COMPATIBILITY.md` | Update status to "Implemented" |

---

## Success Criteria

- [ ] Factory devices (0.0.0) successfully upgrade to newest firmware
- [ ] Legacy devices (2.7.0) skip OTA gracefully and continue working
- [ ] Already-updated devices (2.9.1+) operate normally
- [ ] No regressions in WiFi functionality on any device type
- [ ] Clear log messages for all scenarios

---

## References

- [esp_hosted Component Registry](https://components.espressif.com/components/espressif/esp_hosted)
- [esp-hosted-mcu GitHub](https://github.com/espressif/esp-hosted-mcu)
- [Issue #143: OTA Validation Bug](https://github.com/espressif/esp-hosted-mcu/issues/143)
- [Host Performs Slave OTA Example](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/host_performs_slave_ota)
