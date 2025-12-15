# p3a Web Flasher

Browser-based firmware flasher for the p3a Physical Pixel Art Player.

## How It Works

The web flasher uses [esptool-js](https://github.com/espressif/esptool-js) (Espressif's official JavaScript flashing library) to flash firmware directly from the browser using the Web Serial API.

**Live URL:** https://fabkury.github.io/p3a/web-flasher/

## Files

| File | Description |
|------|-------------|
| `index.html` | Web flasher interface |
| `manifest.json` | Firmware manifest pointing to GitHub Releases |

## Manifest Format

The `manifest.json` file specifies firmware components and their flash addresses:

```json
{
  "name": "p3a Firmware",
  "version": "0.5.4-dev",
  "builds": [
    {
      "chipFamily": "ESP32-P4",
      "parts": [
        { "path": "https://github.com/.../bootloader.bin", "offset": 8192 },
        { "path": "https://github.com/.../partition-table.bin", "offset": 32768 },
        ...
      ]
    }
  ]
}
```

### Flash Addresses (Decimal)

| Component | Hex Address | Decimal Offset |
|-----------|-------------|----------------|
| bootloader.bin | 0x2000 | 8192 |
| partition-table.bin | 0x8000 | 32768 |
| ota_data_initial.bin | 0x10000 | 65536 |
| p3a.bin | 0x20000 | 131072 |
| storage.bin | 0x1020000 | 16908288 |
| network_adapter.bin | 0x1120000 | 17956864 |

## Automatic Updates

When a new release is published on GitHub:

1. The `update-manifest.yml` workflow automatically updates `manifest.json`
2. The `pages.yml` workflow redeploys the web flasher to GitHub Pages

This ensures the web flasher always offers the latest firmware version.

## Manual Manifest Update

If you need to manually update the manifest (e.g., for testing):

1. Edit `docs/web-flasher/manifest.json`
2. Update the `version` field
3. Update all `path` URLs to point to the correct release tag
4. Commit and push to trigger GitHub Pages deployment

## Local Testing

To test the web flasher locally:

1. Start a local HTTP server in the `docs` directory:
   ```bash
   cd docs
   python -m http.server 8000
   ```

2. Open http://localhost:8000/web-flasher/ in Chrome or Edge

3. Note: The firmware files are fetched from GitHub Releases, so internet connection is required

## Browser Requirements

The Web Serial API is required, which is supported in:
- Google Chrome 89+
- Microsoft Edge 89+
- Opera 76+

**Not supported:**
- Firefox (no Web Serial API)
- Safari (no Web Serial API)
- Mobile browsers (iOS/Android)

## Troubleshooting

### "No device found"
- Make sure you're using a USB-C **data** cable (not charge-only)
- Try a different USB port
- On Windows, check Device Manager for the device

### "Failed to connect"
- Hold the BOOT button while clicking "Install"
- Try a different USB cable
- Close other programs that might be using the serial port

### CORS errors
- The manifest and firmware files must be served over HTTPS
- GitHub Releases and GitHub Pages both serve over HTTPS

## Technical Details

- Uses `esptool-js` v0.5.7 from unpkg CDN
- ESP32-P4 chip detection is automatic
- Flash settings: DIO mode, 80MHz, 32MB
- Compression is enabled for faster flashing
