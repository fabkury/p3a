# p3a Flasher

A standalone Windows application for flashing p3a firmware to ESP32-P4 devices.

## Features

- **One-click flashing** — Just connect your device and click Flash
- **Embedded firmware** — Includes the firmware version it was built with
- **Auto-detect devices** — Automatically finds connected ESP32-P4
- **Multiple sources** — Use embedded, GitHub releases, or local ZIP files
- **No installation** — Single portable executable

## For End Users

1. Download `p3a-flasher.exe` from the [Releases page](https://github.com/fabkury/p3a/releases)
2. Connect your p3a device via USB
3. Run `p3a-flasher.exe`
4. Click **Flash Device**
5. Wait for completion (~2 minutes)
6. Done! Your p3a will automatically reboot.

## Build Integration

The flasher is automatically built when you build the p3a project on Windows:

```bash
idf.py build
```

This creates `release/v{VERSION}/p3a-flasher.exe` with the firmware embedded.

### Disabling Flasher Build

To speed up development builds, disable the flasher:

```bash
idf.py build -DP3A_BUILD_FLASHER=OFF
```

Or add to your CMake cache:
```bash
idf.py menuconfig  # or manually edit sdkconfig
```

### Manual Build

To rebuild the flasher separately:

```bash
cd flasher
python build_flasher.py .. 0.6.0-dev ../release/v0.6.0-dev/dev
```

## Development

### Running from Source

```bash
cd flasher
pip install -r requirements.txt
python p3a_flasher.py
```

Note: When running from source without embedded firmware, only "GitHub Release" and "Local ZIP" options are available.

### Project Structure

```
flasher/
├── p3a_flasher.py      # Main GUI application
├── build_flasher.py    # Build script (called by CMake)
├── p3a_logo.png        # Application icon
├── requirements.txt    # Python dependencies
├── build.ps1           # Legacy manual build script
└── README.md           # This file
```

## Technical Details

### Flash Configuration

| Setting | Value |
|---------|-------|
| Chip | ESP32-P4 |
| Baud Rate | 460800 |
| Flash Mode | DIO |
| Flash Frequency | 80 MHz |
| Flash Size | 32 MB |

### Firmware Files

| File | Address | Purpose |
|------|---------|---------|
| bootloader.bin | 0x2000 | Second stage bootloader |
| partition-table.bin | 0x8000 | Flash partition layout |
| ota_data_initial.bin | 0x10000 | OTA boot selection |
| p3a.bin | 0x20000 | Main application |
| storage.bin | 0x1020000 | LittleFS web assets |
| network_adapter.bin | 0x1420000 | ESP32-C6 Wi-Fi firmware (slave_fw partition) |

### Dependencies

- **esptool** — Espressif's flash tool
- **pyserial** — Serial port communication
- **requests** — HTTP client for GitHub API
- **Pillow** — Image handling for logo
- **PyInstaller** — Executable bundling

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "No ports found" | Try a different USB cable (many are charge-only) |
| Device not detected | Hold BOOT button while connecting USB |
| Flash fails | Try again, or check console output for details |
| Antivirus blocks exe | Add exception (false positive due to PyInstaller) |

## License

Part of the [p3a project](https://github.com/fabkury/p3a).
