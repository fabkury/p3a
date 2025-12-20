# Flash p3a Firmware

Flash the p3a firmware to your Waveshare ESP32-P4 board. This is a one-time process — after the initial flash, all future updates are installed wirelessly via `http://p3a.local/ota`.

---

## Option 1: p3a Flasher (Windows) — Recommended

The easiest way to flash your p3a on Windows.

1. Download `p3a-flasher.exe` from the [Releases page](https://github.com/fabkury/p3a/releases)
2. Connect your p3a via USB
3. Run `p3a-flasher.exe`
4. Click **Flash Device**
5. Wait ~2 minutes
6. Done! Your p3a will automatically reboot.

The flasher auto-detects your device and includes the firmware — no installation or configuration needed.

---

## Option 2: Command Line (All Platforms)

For macOS, Linux, or if you prefer the command line on Windows.

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

1. Go to [github.com/fabkury/p3a/releases](https://github.com/fabkury/p3a/releases)
2. Download the firmware `.zip` file from the latest release
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

Wait ~2 minutes for flashing to complete.

### Step 5: Done!

The device will automatically reboot into p3a:
1. The p3a splash screen appears
2. Connect to the `p3a-setup` Wi-Fi network to configure your Wi-Fi
3. Access your p3a at `http://p3a.local/`

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
idf.py build
```

The build automatically creates:
- `release/v{VERSION}/` — Firmware files for distribution
- `release/v{VERSION}/p3a-flasher.exe` — Windows flasher with embedded firmware (if built on Windows)

To disable flasher building during development:
```bash
idf.py build -DP3A_BUILD_FLASHER=OFF
```

Requires [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).
