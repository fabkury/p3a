# SDIO Bus Conflict Analysis

This document analyzes the SDIO bus configuration and potential memory-related causes of the observed conflicts between SD card and WiFi operations.

## The Problem

The p3a device experiences SDIO bus errors approximately 1-2 seconds after MQTT connects:

```
E (11738) H_SDIO_DRV: Dropping packet(s) from stream
E (11738) sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x109
E (11738) H_SDIO_DRV: Failed to push data to rx queue
E (11738) H_SDIO_DRV: sdio_write_task: 0: Failed to send data: 265 66 66
E (11749) sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107
E (11755) H_SDIO_DRV: sdio_write_task: 1: Failed to send data: 263 66 66
E (11762) H_SDIO_DRV: Unrecoverable host sdio state, reset host mcu
```

Error codes:
- `0x109` = ESP_ERR_TIMEOUT (command timed out)
- `0x107` = ESP_ERR_INVALID_RESPONSE (CRC error or no response)

## Hardware Configuration

The ESP32-P4 communicates with two SDIO devices:

### SDIO Slot 0: SD Card
- Standard SD card interface
- Used for artwork storage (`/sdcard/p3a/vault/`)

### SDIO Slot 1: ESP32-C6 Co-processor (esp-hosted)
- WiFi 6 via esp-hosted driver
- 40 MHz clock frequency
- 4-bit bus width
- Streaming mode enabled

## Current SDIO/WiFi Configuration

From `sdkconfig`:

```ini
# esp-hosted SDIO configuration
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000
CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=40
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=40
CONFIG_ESP_HOSTED_HOST_TO_ESP_WIFI_DATA_THROTTLE=y
CONFIG_ESP_HOSTED_PRIV_WIFI_TX_SDIO_HIGH_THRESHOLD=80

# WiFi buffer configuration
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32
```

## Memory Regions and DMA

### Internal SRAM Usage for SDIO/WiFi

WiFi and SDIO operations require DMA-capable buffers in internal SRAM:

| Component | Buffer Type | Estimated Size |
|-----------|-------------|----------------|
| esp-hosted SDIO TX queue | 40 entries | ~variable |
| esp-hosted SDIO RX queue | 40 entries | ~variable |
| WiFi static RX buffers | 10 buffers | ~16 KB |
| WiFi dynamic RX buffers | 32 buffers | ~50 KB |
| WiFi dynamic TX buffers | 32 buffers | ~50 KB |
| LWIP buffers | Various | ~50-100 KB |

**Total WiFi/SDIO**: ~200-300 KB of internal SRAM

### PSRAM DMA Capability

ESP32-P4 supports PSRAM DMA (`CONFIG_SOC_PSRAM_DMA_CAPABLE=y`), but:

1. **WiFi buffers are NOT configured for PSRAM** (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is not set)
2. **esp-hosted internal buffers likely require internal SRAM** for timing-critical operations

## Potential Memory-Related Causes

### Hypothesis 1: Internal RAM Exhaustion

If internal RAM is nearly exhausted, SDIO buffer allocation can fail or cause fragmentation issues.

**Evidence**:
- Many large task stacks in internal RAM (200+ KB)
- Some buffers that could be in PSRAM are using internal RAM

**Mitigation**: Migrate task stacks and large buffers to PSRAM (see RECOMMENDATIONS.md)

### Hypothesis 2: DMA Channel Contention

Both SD card and esp-hosted use DMA for transfers. Heavy simultaneous use could cause:
- DMA channel starvation
- Bus arbitration delays
- Buffer overruns

**Mitigation**:
- Reduce internal RAM pressure to ensure DMA buffers can always be allocated
- Consider increasing DMA channel priority for WiFi (requires ESP-IDF modifications)

### Hypothesis 3: Cache Coherency Issues

With PSRAM DMA (`CONFIG_SOC_AXI_GDMA_SUPPORT_PSRAM=y`), cache coherency becomes critical.

If a buffer in PSRAM is used for DMA without proper cache invalidation/writeback, data corruption can occur.

**Current state**: Code uses `esp_cache_msync()` for display buffers, but WiFi buffers are in internal RAM (no cache issues there).

### Hypothesis 4: Timing Conflicts at MQTT Connect

The crash occurs shortly after MQTT connection. At this moment:
1. TLS handshake completed (many allocations)
2. MQTT subscriptions being sent
3. Initial MQTT messages being received
4. SD card may be accessed for channel data

**Potential issue**: Burst of activity causes RX queue overflow.

**Mitigation**:
- Increase `CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE` (already tried, reportedly no effect)
- Add delays between MQTT connect and initial operations (previously tested)
- Ensure SD card operations pause during MQTT connection burst

## SDIO Bus Serialization in p3a

The codebase already includes SDIO bus locking awareness:

```c
// In animation_player.c - checks for SDIO bus lock
ESP_LOGW(TAG, "Swap request ignored: SDIO bus locked by %s", ...);
```

And serialized download operations:

```c
// In makapix_artwork.c - 128KB chunk downloads
// Each chunk: read WiFi → write to SD (serialized to prevent SDIO conflicts)
```

This suggests the developers are aware of bus contention issues.

## Configuration Experiments to Try

### Experiment 1: Reduce WiFi Buffer Count

```ini
# Try reducing buffer counts to reduce internal RAM pressure
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16  # was 32
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16  # was 32
```

**Risk**: May reduce WiFi throughput.

### Experiment 2: Enable WiFi PSRAM Buffers

```ini
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

**Risk**: May increase WiFi latency. Requires testing.

### Experiment 3: Disable SDIO Streaming Mode

```ini
CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=n  # was y
```

**Note**: This was reportedly already tried without success.

### Experiment 4: Lower SDIO Frequency

```ini
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=20000  # was 40000
```

**Note**: This was reportedly already tried without success.

### Experiment 5: Increase SDIO Queue Sizes Further

```ini
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=80  # was 40
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=80  # was 40
```

**Risk**: More internal RAM usage. May not help if the issue is buffer overflow in the slave.

## Related ESP-IDF Issues

The log message references a known esp-hosted issue:

```
W (9856) slave_ota: Detected esp_hosted 2.7.0 - OTA not possible (confirmed bug)
W (9856) slave_ota: See: https://github.com/espressif/esp-hosted-mcu/issues/143
```

This indicates ESP32-C6 slave firmware 2.7.0 has known issues. The embedded firmware is 2.9.3, but OTA to the slave is blocked by the bug.

**Action**: If possible, manually flash ESP32-C6 with 2.9.3 firmware to test if newer slave firmware resolves the SDIO issues.

## Recommended Investigation Path

1. **Immediate**: Implement Priority 1 PSRAM migrations (task stacks) to maximize free internal RAM
2. **Monitor**: Enable memory reporting (`CONFIG_P3A_MEMORY_REPORTING_ENABLE`) to track internal RAM usage over time
3. **Test**: Try enabling `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` with thorough WiFi stability testing
4. **Investigate**: Check if manually updating ESP32-C6 firmware is feasible
5. **Debug**: Add logging around SDIO operations to correlate with memory allocation patterns

## Memory Reporting

Enable periodic memory reporting to track internal RAM usage:

```ini
CONFIG_P3A_MEMORY_REPORTING_ENABLE=y
```

This will log internal RAM, SPIRAM, and DMA-capable memory every 15 seconds, helping identify memory exhaustion patterns.
