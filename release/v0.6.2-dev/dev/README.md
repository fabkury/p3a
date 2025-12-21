# p3a Firmware v0.6.2-dev

This folder contains all files needed to flash the p3a firmware to an ESP32-P4 device
(e.g., Waveshare ESP32-P4-WiFi6-Touch-LCD board).

## Files Included

| File | Description | Flash Address |
|------|-------------|---------------|
| `p3a.bin` | Main application firmware | 0x20000 |
| `bootloader.bin` | ESP-IDF bootloader | 0x2000 |
| `partition-table.bin` | Partition table | 0x8000 |
| `ota_data_initial.bin` | OTA data (boot selection) | 0x10000 |
| `storage.bin` | SPIFFS web UI assets | 0x1020000 |
| `network_adapter.bin` | ESP32-C6 co-processor firmware | 0x1120000 |

Each `.bin` file has a corresponding `.sha256` file containing its SHA256 checksum.

## Prerequisites

1. **Python 3.x** with `esptool` installed:
   ```
   pip install esptool
   ```

2. **USB driver** for your ESP32-P4 board

3. Know your **serial port** (e.g., `COM5` on Windows, `/dev/ttyUSB0` on Linux)

## Flashing Instructions

### Option 1: Quick Flash (Recommended for updates)

Open a terminal in this folder and run:

```bash
# Windows command prompt (replace COM5 with your port)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args

# Windows PowerShell (replace COM5 with your port) (double quotes required around @flash_args)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force "@flash_args"

# Linux/macOS (replace /dev/ttyUSB0 with your port)
python -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

### Option 2: Full Erase + Flash (Recommended for first flash or major updates)

If you're having issues or this is a fresh device, erase the flash first:

```bash
# Erase all flash (WARNING: erases all saved settings!)
python -m esptool --chip esp32p4 -p COM5 erase_flash

# Then flash
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

### Option 3: Manual Flash (individual files)

Flash each file separately:

```bash
python -m esptool --chip esp32p4 -p COM5 -b 460800 write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force \
    0x2000 bootloader.bin \
    0x8000 partition-table.bin \
    0x10000 ota_data_initial.bin \
    0x20000 p3a.bin \
    0x1020000 storage.bin \
    0x1120000 network_adapter.bin
```

Note: `--force` is needed because `network_adapter.bin` is for ESP32-C6, not ESP32-P4.
It's stored in a data partition and transferred to the co-processor via OTA at boot.

## Verifying the Flash

After flashing, the device should:
1. Display the p3a splash screen
2. Connect to WiFi (if previously configured) or start a captive portal
3. Be accessible at http://p3a.local/

## Troubleshooting

- **"Failed to connect"**: Hold the BOOT button while pressing RST, then release BOOT
- **"Permission denied"**: Run terminal as administrator or check USB permissions
- **Device not responding**: Try erasing flash first, then reflashing
- **Wrong port**: List ports with `python -m serial.tools.list_ports`

## Checksum Verification

To verify file integrity:

```bash
# Windows PowerShell
Get-FileHash p3a.bin -Algorithm SHA256 | Select-Object Hash

# Linux/macOS
sha256sum p3a.bin
```

Compare with contents of `p3a.bin.sha256`.
