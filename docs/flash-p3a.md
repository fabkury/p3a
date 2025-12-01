# Flash p3a Firmware

This guide explains how to install the p3a firmware on your Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board.

## What you need

- **Hardware**: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B and a microSD card
- **USB cable**: A USB-C cable that supports data transfer (charging-only cables won't work)
- **Computer**: Windows, macOS, or Linux

---

## Option 1: Web Flasher (Coming Soon)

> **Status:** The browser-based web flasher is currently under development and will be available soon. Once ready, you'll be able to flash directly from your browser without installing any tools—just plug in your device and click a button.
>
> Bookmark this page and check back for updates, or use the manual method below in the meantime.

---

## Option 2: Flash with esptool.py

This method works on any operating system and uses Espressif's official flashing tool.

### Step 1: Install esptool

If you have Python installed:

```bash
pip install esptool
```

Or use the copy bundled with ESP-IDF if you have that installed.

### Step 2: Download the firmware files

Download these files from the [releases page](https://github.com/fabkury/p3a/releases) or the `build/` folder:

- `bootloader/bootloader.bin`
- `partition_table/partition-table.bin`
- `p3a.bin`
- `storage.bin`

### Step 3: Connect the board

1. Locate the USB-C ports on your board
2. Connect the **Full-Speed (FS) port** to your computer—this is the flashing port
3. The screen may stay dark during flashing; that's normal

### Step 4: Flash the firmware

Run this command, adjusting the port as needed:

```bash
esptool.py --chip esp32p4 --port COM3 --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB \
  0x2000 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 p3a.bin \
  0x810000 storage.bin
```

**Port names by OS:**
- **Windows**: `COM3`, `COM4`, etc. (check Device Manager)
- **macOS**: `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`
- **Linux**: `/dev/ttyUSB0` or `/dev/ttyACM0`

### Step 5: Reboot and enjoy

Once flashing completes:
1. Disconnect the USB cable
2. Insert a microSD card with artwork (see [HOW-TO-USE.md](HOW-TO-USE.md#preparing-artwork))
3. Reconnect power via USB-C
4. Follow the Wi-Fi setup on the screen

---

## Option 3: Build from Source

For developers who want to customize the firmware:

### Prerequisites

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) with esp32p4 target
- Git

### Build and flash

```bash
# Clone the repository
git clone https://github.com/fabkury/p3a.git
cd p3a

# Set target (first time only)
idf.py set-target esp32p4

# Optional: customize settings
idf.py menuconfig

# Build and flash
idf.py build flash monitor
```

Use `-p PORT` to specify the serial port if auto-detection fails.

### Configuration options

Run `idf.py menuconfig` to access options under "Physical Player of Pixel Art (P3A)":

- **Auto-swap interval**: How often to change artwork automatically
- **Animation directory**: Where to look for artwork on SD card
- **Touch sensitivity**: Gesture thresholds
- **PICO-8 Monitor**: Enable/disable the PICO-8 streaming feature
- **USB Mass Storage**: Enable/disable USB SD card access

---

## Troubleshooting

### "No serial port found" or similar

- Try a different USB cable (many cables are charge-only)
- Try a different USB port on your computer
- On Windows, check Device Manager for the COM port number
- On Linux, you may need to add yourself to the `dialout` group: `sudo usermod -a -G dialout $USER`

### "Failed to connect" or timeout errors

- Make sure you're using the correct USB-C port (FS port for flashing)
- Try holding the BOOT button while connecting
- Reduce baud rate: replace `460800` with `115200`
- Close any other programs that might be using the serial port

### Flashing succeeds but device doesn't boot

- Verify all four files were flashed to the correct addresses
- Try erasing flash first: `esptool.py --chip esp32p4 erase_flash`
- Re-flash all files

### Need help?

- Check the [INFRASTRUCTURE.md](INFRASTRUCTURE.md) for technical details
- Open an issue on the [GitHub repository](https://github.com/fabkury/p3a)
