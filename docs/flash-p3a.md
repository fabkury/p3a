# Flash p3a Firmware

Flash the p3a firmware to your Waveshare ESP32-P4 board. This is a one-time process — after the initial flash, all future updates are installed wirelessly via `http://p3a.local/ota`.

---

## Quick Start (5 minutes)

### Step 1: Install Python and esptool

**Windows:**
1. Download Python from [python.org](https://www.python.org/downloads/)
2. During installation, check **"Add Python to PATH"**
3. Open PowerShell and run:
   ```powershell
   pip install esptool
   ```

**macOS:**
```bash
brew install python
pip3 install esptool
```

**Linux:**
```bash
sudo apt install python3 python3-pip
pip3 install esptool
```

### Step 2: Download firmware

Download and extract the latest release:
1. Go to [github.com/fabkury/p3a/releases](https://github.com/fabkury/p3a/releases)
2. Download the `.zip` file from the latest release
3. Extract to a folder (e.g., `p3a-firmware`)

### Step 3: Connect the board

1. Use a **USB-C data cable** (charging-only cables won't work)
2. Connect to the **Full-Speed (FS) USB-C port** on your board
3. Find your serial port:
   - **Windows:** Open Device Manager → Ports → Note the COM number (e.g., `COM5`)
   - **macOS:** Run `ls /dev/cu.usb*` in Terminal
   - **Linux:** Run `ls /dev/ttyUSB*` or `ls /dev/ttyACM*`

### Step 4: Flash

Open a terminal in the firmware folder and run one command:

**Windows PowerShell:** (replace `COM5` with your port)
```powershell
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force "@flash_args"
```

**Windows Command Prompt:**
```cmd
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

**macOS/Linux:** (replace `/dev/ttyUSB0` with your port)
```bash
python3 -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args
```

Wait ~2 minutes for flashing to complete. You'll see progress for each file.

### Step 5: Done!

1. Disconnect and reconnect the USB cable
2. The p3a splash screen appears
3. Connect to the `p3a-setup` Wi-Fi network to configure your Wi-Fi
4. Access your p3a at `http://p3a.local/`

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "No serial port found" | Try a different USB cable — many are charge-only |
| "Failed to connect" | Hold the **BOOT** button while connecting |
| Permission denied (Linux) | Run: `sudo usermod -a -G dialout $USER` then log out/in |
| Wrong COM port (Windows) | Check Device Manager → Ports |
| Timeout errors | Try `-b 115200` instead of `-b 460800` |

---

## Why not a web flasher?

The browser-based esptool-js library has a [known limitation](https://github.com/espressif/esptool-js/issues) where it cannot properly configure flash settings for ESP32-P4 chips. We're tracking this issue with Espressif and will enable web flashing once it's resolved.

---

## After flashing

### Wireless updates (OTA)

After the initial flash, update wirelessly:
1. Open `http://p3a.local/ota` in your browser
2. Click "Check for updates"
3. Click "Install" if an update is available

### Adding artwork

Copy WebP, GIF, PNG, or JPEG files to an `animations` folder on a microSD card, then insert it into your p3a.

See [HOW-TO-USE.md](HOW-TO-USE.md) for detailed instructions.

---

## Advanced: Build from source

For developers who want to customize the firmware:

```bash
# Clone and build
git clone https://github.com/fabkury/p3a.git
cd p3a
idf.py set-target esp32p4
idf.py build flash monitor
```

Requires [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).
