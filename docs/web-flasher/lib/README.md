# esptool-js Bundle

This folder contains a custom build of esptool-js from **PR #226**.

## Source

- **Repository**: https://github.com/espressif/esptool-js
- **Pull Request**: https://github.com/espressif/esptool-js/pull/226
- **Title**: "use uint8array instead of string for write flash command"

## Why a Custom Build?

The official npm release of esptool-js (v0.5.7) uses binary strings for firmware data, which causes corruption when flashing ESP32-P4 devices. PR #226 adds native `Uint8Array` support, which preserves binary data correctly.

## How to Rebuild

If you need to update this bundle:

```bash
# Clone esptool-js
git clone https://github.com/espressif/esptool-js.git
cd esptool-js

# Fetch and checkout PR #226
git fetch origin pull/226/head:pr-226
git checkout pr-226

# Install dependencies and build
npm install
npm run build

# Copy the bundle
cp bundle.js /path/to/p3a/docs/web-flasher/lib/esptool-bundle.js
```

## When to Remove

Once PR #226 is merged and released to npm, we can switch back to using the official npm package via CDN and remove this custom build.

## Build Date

Built: December 20, 2025

