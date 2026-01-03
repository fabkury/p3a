# p3a Firmware v0.6.5-dev

This folder contains all files needed to flash the p3a firmware to an ESP32-P4 device
(e.g., Waveshare ESP32-P4-WiFi6-Touch-LCD board).

After the initial flash, all future updates are installed wirelessly via `http://p3a.local/ota`.

---

## Option 1: p3a Flasher (Windows) — Recommended

The easiest way to flash your p3a on Windows.

1. Connect your p3a via USB-C
2. Run `p3a-flasher.exe` (included in this folder)
3. Click **Flash Device**
4. Wait ~2 minutes
5. Done! Your device will automatically reboot into p3a.

The flasher auto-detects your device and includes the firmware — no installation, configuration or Internet connection needed.

---

## Option 2: Command Line (All Platforms)

For macOS, Linux, or if you prefer the command line on Windows.

### Prerequisites

1. **Python 3.x** with `esptool` installed:
   ```
   pip install esptool
   ```

2. **USB driver** for your ESP32-P4 board

3. Know your **serial port** (e.g., `COM5` on Windows, `/dev/ttyUSB0` on Linux)

### Quick Flash

Open a terminal in this folder and run:

```bash
# Windows PowerShell (replace COM5 with your port)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB --force "@flash_args"

# Windows Command Prompt (replace COM5 with your port)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB --force @flash_args

# Linux/macOS (replace /dev/ttyUSB0 with your port)
python -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB --force @flash_args
```

### Full Erase + Flash (if having issues)

If you're having issues or this is a fresh device, erase the flash first:

```bash
# Erase all flash (WARNING: erases all saved settings!)
python -m esptool --chip esp32p4 -p COM5 erase_flash

# Then flash
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB --force @flash_args
```

---

## After Flashing

The device will automatically reboot into p3a:
1. The p3a splash screen appears
2. Connect to the `p3a-setup` Wi-Fi network to configure your Wi-Fi
3. Access your p3a at `http://p3a.local/`

---

## Files Included

| File | Description | Flash Address |
|------|-------------|---------------|
| `p3a-flasher.exe` | Windows flasher (recommended) | — |
| `p3a.bin` | Main application firmware | 0x20000 |
| `bootloader.bin` | ESP-IDF bootloader | 0x2000 |
| `partition-table.bin` | Partition table | 0x8000 |
| `ota_data_initial.bin` | OTA data (boot selection) | 0x10000 |
| `storage.bin` | LittleFS web UI assets (4 MB) | 0x1020000 |
| `network_adapter.bin` | ESP32-C6 co-processor firmware | 0x1420000 |

Each `.bin` file has a corresponding `.sha256` file containing its SHA256 checksum.

Note: `--force` is needed for command-line flashing because `network_adapter.bin` is for ESP32-C6, not ESP32-P4.
It's stored in a data partition and transferred to the co-processor via OTA at boot.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "No serial port found" | Try a different USB cable - many are charge-only |
| "Failed to connect" | Hold the **BOOT** button while connecting |
| Permission denied (Linux) | Run: `sudo usermod -a -G dialout $USER` then log out/in |
| Wrong COM port (Windows) | Check Device Manager > Ports |
| Timeout errors | Try `-b 115200` instead of `-b 460800` |

---

## Checksum Verification

To verify file integrity:

```bash
# Windows PowerShell
Get-FileHash p3a.bin -Algorithm SHA256 | Select-Object Hash

# Linux/macOS
sha256sum p3a.bin
```

Compare with contents of `p3a.bin.sha256`.
