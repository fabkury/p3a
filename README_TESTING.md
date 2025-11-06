# Testing Instructions

## WebP Animation Files

The WebP animation files have been copied to `firmware/animations/`:
- `decoded_3826495.webp`
- `idea11-ionic-connections-128p-L92-b809.webp`
- `ms-black-dragon-64p-L82.webp`
- `r3-artificer-32p-L55.webp`

These files need to be placed on the SD card in one of these locations:
- `/sdcard/animations/` (preferred)
- `/sdcard/` (fallback)

## Building and Testing

1. **Activate ESP-IDF environment** (if not already active):
   ```powershell
   . $env:USERPROFILE\.espressif\esp-idf\export.ps1
   ```

2. **Build the firmware**:
   ```powershell
   cd firmware
   idf.py build
   ```

3. **Flash the firmware**:
   ```powershell
   idf.py -p COM5 flash
   ```
   (Replace COM5 with your actual serial port)

4. **Copy animation files to SD card**:
   - Insert SD card into the device
   - Create `animations` folder on the SD card
   - Copy all `.webp` files from `firmware/animations/` to the SD card's `animations` folder

5. **Monitor the output**:
   ```powershell
   python ../tools/serial_monitor.py --port COM5 --duration 60
   ```

## Expected Behavior

1. **On boot**: The firmware should scan for WebP files and log which animations were found
2. **Animation playback**: The first animation should start playing automatically in a loop
3. **Touch interaction**: Tapping the screen should cycle to the next animation
4. **FPS display**: A red FPS counter should appear in the top-right corner

## Troubleshooting

- If no animations are found, check the SD card mount status in the logs
- If animations don't play, verify the WebP files are valid and on the SD card
- Check logs for any WebP decoder errors

