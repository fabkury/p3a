# Flash p3a from Your Browser

This page is written for people who just want their Pixel Pea device to work—no firmware knowledge needed.

- **Required hardware**:
  - Waveshare ESP32-P4-WIFI6-Touch-LCD-4B,
  - USB-C cable that supports data,
  - and a microSD card.
- **Computer**: Windows, macOS, Linux, or Chromebook with the latest Chrome or Microsoft Edge. (Android Chrome works in most cases; iOS/Safari does **not** expose Web Serial yet.)

---

## 1. Plug everything in
1. Turn the device so you can reach the USB-C ports on the back/top edge.
2. Use a USB-C cable that you know can transfer data (chargers-only cables will not work).
3. Connect it to your laptop/desktop. The screen can stay dark—that is normal while we flash new firmware.

## 2. Open the Web Flasher

Click the button below. It opens Espressif's official Web Flasher, already preloaded with the correct p3a images.

<p align="center">
  <a href="https://espressif.github.io/web-tools/flash?flash_config_url=https://raw.githubusercontent.com/fabkury/p3a/main/docs/web-flasher/p3a-esp32p4.json" target="_blank" rel="noopener">
    <img src="https://img.shields.io/badge/Open%20Web%20Flasher-ESP32--P4-blue?style=for-the-badge" alt="Open Web Flasher">
  </a>
</p>

Keep this guide open—we will refer back to it in the next steps.

## 3. Let the browser do the work

1. In the Web Flasher tab, click **Connect**.
2. A popup will list every serial/USB device currently attached. Pick the entry that mentions `USB Serial` (on Windows it is usually `COMx`, on macOS/Linux it starts with `/dev/tty.usbmodem` or `/dev/ttyACM`).
3. After granting access, click **Install Pixel Pea Firmware** (or the button named similarly in the UI).
4. Wait while the progress bar goes through four files: bootloader, partition table, firmware, and storage image. The whole process typically finishes in under two minutes.
5. When it says **Done**, disconnect the USB cable and power-cycle the device. The screen should now show the Pixel Pea boot logo followed by animation playback.

### MicroSD reminder

The firmware expects a microSD card with artwork. If you have not prepared one yet, copy some GIF/WebP/PNG/JPEG animations into an `animations` folder on the card, then insert it before rebooting.

---

## Frequently asked questions

**The connect dialog is empty or my device does not show up.**  
- Try a different USB port or cable (most failures are power-only cables).  
- Close other apps that might already be using the serial port (Arduino, VS Code, etc.).  
- On Windows, confirm that the device manager lists “USB Serial Device (COMx)”.

**The flashing process fails halfway through.**  
- Unplug the board, wait five seconds, plug it back in, and run the flasher again.  
- Ensure no other peripherals are drawing power from the same USB hub.  
- If it keeps failing, fall back to the manual `esptool.py` method described later in the repository README.

**Are phones supported?**  
- Chrome on Android usually works (you will get the same serial-port chooser).  
- iOS/Safari currently does not expose Web Serial, so you need a desktop OS.

**How do I know it really flashed something?**  
- The console pane in the flasher prints `Writing at 0x...` for each region.  
- When it reaches 100 %, the board automatically reboots; you will hear the Windows USB disconnect sound or see the LED blink.

---

## Need a deeper dive?

The `README.md` still documents how to build from source or flash manually with `esptool.py`. Use that path if you want to tweak `menuconfig`, modify the code, or flash over a slow/unsupported browser.

