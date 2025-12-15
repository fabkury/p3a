# Flash p3a Firmware

This guide explains how to install the p3a firmware on your Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board.

> **Note:** You only need to flash via USB-C once. After the initial installation, all future firmware updates can be installed wirelessly through the web interface at `http://p3a.local/ota`. See [HOW-TO-USE.md](HOW-TO-USE.md#firmware-updates) for details.

## What you need

- **Hardware**: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B and a microSD card
- **USB cable**: A USB-C cable that supports data transfer (charging-only cables won't work)
- **Computer**: Windows, macOS, or Linux

---

## Option 1: Web Flasher (Recommended)

The easiest way to flash p3a firmware is directly from your browser—no installation required.

### Requirements
- **Browser**: Google Chrome 89+ or Microsoft Edge 89+ (Firefox and Safari are not supported)
- **Operating system**: Windows, macOS, Linux, or ChromeOS (mobile devices are not supported)

### Steps

1. Connect your ESP32-P4 board to your computer using the **Full-Speed (FS) USB-C port**
2. Open the **[p3a Web Flasher](https://fabkury.github.io/p3a/web-flasher/)**
3. Click **"Install p3a Firmware"**
4. Select your device from the popup (it may appear as "USB JTAG/serial debug unit" or similar)
5. Wait for the flashing to complete (~2-3 minutes)
6. Disconnect and reboot your device

That's it! Your p3a is now ready to use.

> **Troubleshooting:** If you don't see your device in the list, try a different USB cable or USB port. Many cables are charge-only and don't support data transfer.

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

Download all `.bin` files from the [latest release](https://github.com/fabkury/p3a/releases/latest):

- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin`
- `p3a.bin`
- `storage.bin`
- `network_adapter.bin`

### Step 3: Connect the board

1. Locate the USB-C ports on your board
2. Connect the **Full-Speed (FS) port** to your computer—this is the flashing port
3. The screen may stay dark during flashing; that's normal

### Step 4: Flash the firmware

Run this command from the folder containing your downloaded files, adjusting the port as needed:

```bash
python -m esptool --chip esp32p4 -p COM5 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force \
  0x2000 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin \
  0x20000 p3a.bin \
  0x1020000 storage.bin \
  0x1120000 network_adapter.bin
```

**Port names by OS:**
- **Windows**: `COM3`, `COM4`, etc. (check Device Manager)
- **macOS**: `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`
- **Linux**: `/dev/ttyUSB0` or `/dev/ttyACM0`

> **Note:** The `--force` flag is required because `network_adapter.bin` is ESP32-C6 firmware stored in a data partition for the co-processor.

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

### Web Flasher shows "Browser Not Supported"

- Use Google Chrome or Microsoft Edge on desktop
- Firefox, Safari, and mobile browsers do not support Web Serial API
- Make sure you're not in incognito/private browsing mode

### Flashing succeeds but device doesn't boot

- Verify all six files were flashed to the correct addresses
- Try erasing flash first: `python -m esptool --chip esp32p4 -p COM5 erase_flash`
- Re-flash all files

### Need help?

- Check the [INFRASTRUCTURE.md](INFRASTRUCTURE.md) for technical details
- Open an issue on the [GitHub repository](https://github.com/fabkury/p3a)

---

## After the First Flash

### Over-the-Air Updates

Once your p3a is flashed and connected to Wi-Fi, you never need to connect a USB cable again for firmware updates. All subsequent updates can be installed wirelessly:

1. Open `http://p3a.local/ota` in your browser
2. Check for available updates
3. Click "Install" to download and install the update
4. The device reboots automatically when complete

See [HOW-TO-USE.md](HOW-TO-USE.md#firmware-updates) for detailed instructions.

### ESP32-C6 Co-processor Firmware

The Waveshare board has two processors:
- **ESP32-P4**: Main processor (runs p3a firmware)
- **ESP32-C6**: Wi-Fi/Bluetooth co-processor (runs ESP-Hosted firmware)

**You don't need to flash the ESP32-C6 separately.** The p3a firmware automatically detects when the co-processor firmware needs updating and flashes it during boot. This happens automatically when:
- The ESP32-C6 firmware is outdated
- The ESP32-C6 firmware is missing or corrupted

The auto-flash process takes about 30 seconds. You'll see a progress indicator on the display. The device continues to normal operation once complete.
