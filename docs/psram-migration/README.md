# PSRAM Migration Analysis for p3a

This folder contains a comprehensive analysis of memory allocation patterns in the p3a codebase, with a focus on identifying opportunities to migrate from internal RAM to PSRAM. The primary goal is to reduce internal RAM pressure, which may be contributing to SDIO bus conflicts between the SD card and the ESP32-C6 WiFi co-processor.

## Background

The p3a device experiences intermittent SDIO bus errors when both the SD card and WiFi (via esp-hosted over SDIO) are actively transferring data. The errors manifest as:

```
E (11738) H_SDIO_DRV: Dropping packet(s) from stream
E (11738) sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x109
E (11738) H_SDIO_DRV: Failed to push data to rx queue
E (11762) H_SDIO_DRV: Unrecoverable host sdio state, reset host mcu
```

Previous mitigation attempts (increasing SDIO RX queue size, disabling streaming mode, reducing SDIO frequency) have not resolved the issue.

## Hypothesis

Internal RAM contention may be exacerbating SDIO bus conflicts. By moving large allocations to PSRAM, we can:

1. Reduce DMA buffer pressure in internal RAM
2. Increase available internal RAM for WiFi/SDIO buffers
3. Potentially reduce bus arbitration conflicts

## Document Structure

| Document | Contents |
|----------|----------|
| [FINDINGS.md](FINDINGS.md) | Detailed inventory of all memory allocations |
| [RECOMMENDATIONS.md](RECOMMENDATIONS.md) | Specific migration recommendations by priority |
| [SDIO-ANALYSIS.md](SDIO-ANALYSIS.md) | Analysis of SDIO-specific configurations and buffers |

## Current Memory Configuration

From `sdkconfig`:

| Setting | Value | Impact |
|---------|-------|--------|
| `SPIRAM` | Enabled | PSRAM available for allocations |
| `SPIRAM_SPEED` | 200 MHz | Fast PSRAM access |
| `SPIRAM_USE_MALLOC` | Yes | malloc() can use PSRAM |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 16384 | Allocations < 16KB stay internal |
| `SPIRAM_MALLOC_RESERVE_INTERNAL` | 32768 | Reserve 32KB for internal-only |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | **No** | WiFi buffers stay in internal RAM |
| `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | Yes | Task stacks can use PSRAM |

## Quick Summary

### High-Impact Migration Candidates

1. **Animation Frame Buffers** (~3+ MB) - Currently using `malloc()`, should explicitly use PSRAM
2. **Download Manager Task Stack** (80 KB) - Currently internal, should use PSRAM static stack
3. **Channel Cache Arrays** (up to 64 KB) - Using `malloc()`, should explicitly use PSRAM
4. **Decoder Frame Buffers** (variable) - Using `malloc()`, should explicitly use PSRAM

### Already PSRAM-Aware

- Loader service file loading (has PSRAM preference)
- MQTT reassembly buffer (has PSRAM preference)
- Makapix artwork download chunks (has PSRAM preference)
- PICO-8 frame buffers (has PSRAM preference)
- Channel refresh task stack (pre-allocated in PSRAM)

### Must Stay in Internal RAM

- Upscale lookup tables (fast CPU access required)
- Display DMA buffers (managed by ESP-IDF MIPI-DSI driver)
- WiFi TX/RX buffers (unless CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP enabled)

## Related Documentation

- [docs/INFRASTRUCTURE.md](../INFRASTRUCTURE.md) - Overall system architecture
- ESP-IDF SPIRAM documentation: https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32p4/api-guides/external-ram.html
