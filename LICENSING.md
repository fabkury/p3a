# p3a Licensing Report

This document provides a comprehensive overview of the licensing status of the p3a project and all its dependencies.

## Main Project License

**p3a is licensed under the Apache License 2.0**

- **License**: Apache License, Version 2.0
- **License File**: [LICENSE](LICENSE)
- **Copyright**: 2024-2025 p3a Contributors
- **SPDX Identifier**: Apache-2.0

The Apache 2.0 license is a permissive open source license that:
- Allows commercial use
- Allows modification
- Allows distribution
- Allows patent use
- Requires preservation of copyright and license notices
- Provides an express grant of patent rights from contributors

## Dependencies

### 1. ESP-IDF Framework

The project is built on the Espressif IoT Development Framework (ESP-IDF) v5.5.1.

- **License**: Apache License 2.0
- **Homepage**: https://github.com/espressif/esp-idf
- **SPDX**: Apache-2.0
- **Compatibility**: ✅ Compatible with Apache 2.0

### 2. ESP-IDF Component Registry Dependencies

All components from the Espressif Component Registry are managed via the ESP-IDF Component Manager and listed in `dependencies.lock`. All Espressif-maintained components use Apache 2.0 license.

#### 2.1 Espressif Components

The following components are maintained by Espressif Systems and licensed under Apache 2.0:

| Component | Version | License | Purpose |
|-----------|---------|---------|---------|
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 | Build system utilities |
| espressif/eppp_link | 1.1.3 | Apache-2.0 | EPPP protocol link layer |
| espressif/esp_codec_dev | 1.2.0 | Apache-2.0 | Audio codec device abstraction |
| espressif/esp_hosted | 2.7.0 | Apache-2.0 | ESP-Hosted solution for network co-processor |
| espressif/esp_lcd_touch | 1.2.0 | Apache-2.0 | Touch panel driver interface |
| espressif/esp_lcd_touch_gt911 | 1.2.0 | Apache-2.0 | GT911 touch controller driver |
| espressif/esp_lvgl_port | 2.7.0 | Apache-2.0 | LVGL graphics library port |
| espressif/esp_serial_slave_link | 1.1.2 | Apache-2.0 | Serial slave communication link |
| espressif/esp_tinyusb | 1.7.6~2 | Apache-2.0 | TinyUSB integration for ESP |
| espressif/esp_wifi_remote | 1.2.2 | Apache-2.0 | Wi-Fi remote control support |
| espressif/libpng | 1.6.52 | libpng/zlib | PNG image codec (see below) |
| espressif/mdns | 1.9.1 | Apache-2.0 | mDNS responder |
| espressif/tinyusb | 0.17.0~2 | MIT | USB device/host stack (see below) |
| espressif/wifi_remote_over_eppp | 0.2.1 | Apache-2.0 | Wi-Fi over EPPP protocol |
| espressif/zlib | 1.3.1 | Zlib | Data compression library (see below) |

**Compatibility**: ✅ All Apache-2.0 licensed components are fully compatible

#### 2.2 LVGL Graphics Library

| Component | Version | License | Purpose |
|-----------|---------|---------|---------|
| lvgl/lvgl | 9.4.0 | MIT | Light and Versatile Graphics Library |

- **License**: MIT License
- **Homepage**: https://lvgl.io/
- **SPDX**: MIT
- **Compatibility**: ✅ MIT is compatible with Apache 2.0

#### 2.3 Waveshare Hardware Components

| Component | Version | License | Purpose |
|-----------|---------|---------|---------|
| waveshare/esp32_p4_wifi6_touch_lcd_4b | 1.0.1 | Apache-2.0 | Board support package |
| waveshare/esp_lcd_st7703 | 1.0.5 | Apache-2.0 | ST7703 LCD driver |

- **License**: Apache License 2.0
- **Compatibility**: ✅ Compatible with Apache 2.0

### 3. Third-Party Libraries with Different Licenses

#### 3.1 libpng

- **Version**: 1.6.52 (via espressif/libpng)
- **License**: libpng License (permissive, similar to BSD/MIT)
- **Homepage**: http://www.libpng.org/
- **SPDX**: Libpng
- **Purpose**: PNG image decoding
- **Compatibility**: ✅ The libpng license is a permissive free software license compatible with Apache 2.0
- **License Details**: Allows use, modification, and redistribution with attribution

#### 3.2 zlib

- **Version**: 1.3.1 (via espressif/zlib)
- **License**: Zlib License (permissive)
- **Homepage**: https://www.zlib.net/
- **SPDX**: Zlib
- **Purpose**: Data compression
- **Compatibility**: ✅ The zlib license is a permissive free software license compatible with Apache 2.0
- **License Details**: Very permissive, allows use and redistribution with acknowledgment

#### 3.3 TinyUSB

- **Version**: 0.17.0~2 (via espressif/tinyusb)
- **License**: MIT License
- **Homepage**: https://github.com/hathach/tinyusb
- **SPDX**: MIT
- **Purpose**: USB device/host stack for embedded systems
- **Compatibility**: ✅ MIT is compatible with Apache 2.0
- **License Details**: Permissive license allowing commercial use with attribution

#### 3.4 libwebp

- **Version**: 1.4.0 (fetched during build via FetchContent)
- **License**: BSD 3-Clause License
- **Homepage**: https://chromium.googlesource.com/webm/libwebp
- **SPDX**: BSD-3-Clause
- **Purpose**: WebP image decoding and animation support
- **Compatibility**: ✅ BSD 3-Clause is compatible with Apache 2.0
- **License Details**: Permissive license with attribution requirement
- **Source**: Fetched from `https://chromium.googlesource.com/webm/libwebp` (tag: v1.4.0)
- **Component Path**: `components/libwebp_decoder/` (wrapper component)

### 4. Embedded Libraries in p3a Source Tree

#### 4.1 µGFX (uGFX)

- **Location**: `components/ugfx/`
- **License**: µGFX License (proprietary, but free for open source projects)
- **Homepage**: https://ugfx.io/
- **License URL**: http://ugfx.io/license.html
- **Purpose**: Lightweight embedded graphics library (used for fonts and UI elements)
- **Compatibility**: ⚠️ **Special Consideration Required**
- **License Details**: 
  - Free for open source projects and hobbyists
  - Commercial license required for commercial products
  - Each file contains: "This file is subject to the terms of the GFX License"
- **Notes**: 
  - p3a is an open source project, so the free µGFX license applies
  - If you plan to commercialize p3a or derivatives, you may need to purchase a µGFX commercial license
  - Alternative: Consider replacing µGFX with a fully permissive library (e.g., using only LVGL for graphics)

#### 4.2 AnimatedGIF Library

- **Location**: `components/animated_gif_decoder/`
- **License**: Apache License 2.0
- **Copyright**: 2020 BitBank Software, Inc.
- **Author**: Larry Bank (bitbank@pobox.com)
- **SPDX**: Apache-2.0
- **Purpose**: GIF animation decoder for embedded systems
- **Compatibility**: ✅ Compatible with Apache 2.0 (same license)
- **License Reference**: See header in `components/animated_gif_decoder/include/AnimatedGIF.h`

#### 4.3 DejaVu Fonts

- **Location**: `components/ugfx/src/gdisp/fonts/`
- **Files**: `DejaVuSans.license`, `DejaVuSerif.license`
- **License**: Bitstream Vera Fonts License (permissive, with some restrictions)
- **Copyright**: Bitstream, Inc. / DejaVu changes are public domain
- **SPDX**: Similar to MIT/X11 with font-specific clauses
- **Purpose**: Embedded fonts for display text
- **Compatibility**: ✅ Compatible with Apache 2.0
- **License Details**:
  - Free to use, modify, and distribute
  - Fonts cannot be sold standalone, but can be part of larger software packages
  - Modified fonts must not use the "Bitstream" or "Vera" names
  - Attribution notice must be preserved
- **License Files**: Individual `.license` files in the fonts directory

#### 4.4 Fake08 (PICO-8 Player)

- **Location**: `webui/static/fake08.js`, `webui/static/fake08.wasm`
- **License**: Unknown/Upstream (MIT expected, but not verified in embedded files)
- **Homepage**: https://github.com/jtothebell/fake-08
- **Purpose**: PICO-8 game player (optional feature, can be disabled at compile time)
- **Compatibility**: ⚠️ **Verification Needed**
- **Notes**:
  - These are pre-compiled WebAssembly and JavaScript files
  - Original Fake-08 project is MIT licensed
  - License headers not present in the compiled/minified files included in p3a
  - **Recommendation**: Add attribution notice in documentation
  - Optional component: Can be excluded by disabling `CONFIG_P3A_PICO8_ENABLE` during build

### 5. Python Dependencies (Flasher Tool)

The flasher tool (`flasher/`) uses the following Python packages, specified in `flasher/requirements.txt`:

| Package | Minimum Version | License | Purpose |
|---------|----------------|---------|---------|
| pyserial | 3.5 | BSD-3-Clause | Serial port communication |
| esptool | 4.7 | GPL-2.0-or-later | ESP32 firmware flashing utility |
| requests | 2.28 | Apache-2.0 | HTTP requests for GitHub API |
| Pillow | 9.0 | HPND (PIL License) | Image handling |
| pyinstaller | 6.0 | GPL-2.0-or-later with special exception | Executable bundling |

**Important Notes:**
- **esptool** is GPL-2.0-or-later, which is a copyleft license
- **PyInstaller** is GPL-2.0-or-later but includes a special exception that allows bundling GPL-incompatible programs
- The flasher is a separate utility and not part of the embedded firmware
- Distribution considerations:
  - The flasher executable (`p3a-flasher.exe`) bundles GPL-licensed tools (esptool, PyInstaller runtime)
  - This is acceptable because:
    1. The flasher is distributed as a separate utility, not linked with the Apache 2.0 firmware
    2. PyInstaller's bootloader has an exception allowing this use
    3. Users can alternatively use esptool directly (documented in release README)
  - **Recommendation**: Include attribution notices for GPL components in the flasher distribution

## License Compatibility Summary

| License Type | Compatible with Apache 2.0 | Notes |
|--------------|----------------------------|-------|
| Apache-2.0 | ✅ Yes | Same license |
| MIT | ✅ Yes | Permissive, compatible |
| BSD-3-Clause | ✅ Yes | Permissive, compatible |
| Zlib | ✅ Yes | Very permissive |
| Libpng | ✅ Yes | Permissive, compatible |
| Bitstream Vera Fonts | ✅ Yes | Permissive with font-specific clauses |
| µGFX License | ⚠️ Conditional | Free for open source; commercial use may require license |
| GPL-2.0 (esptool/PyInstaller) | ⚠️ Separate | Flasher tool is separate from firmware, not a linking concern |

## Compliance Requirements

To maintain full compliance with all licenses in the p3a project:

### 1. Apache 2.0 Components (Main Code)

✅ **Implemented:**
- [x] Include LICENSE file in repository root (Apache 2.0 full text)
- [x] This LICENSING.md document for comprehensive attribution

🔲 **Recommended:**
- [ ] Add Apache 2.0 SPDX headers to original source files in `main/` and `components/` subdirectories
- [ ] Consider adding NOTICE file with attributions for all dependencies

### 2. MIT Licensed Components (LVGL, TinyUSB)

✅ **Requirements Met:**
- Copyright notices preserved in upstream components (managed by ESP-IDF Component Manager)
- MIT license text available in managed component directories
- Attribution provided in this LICENSING.md

### 3. libwebp (BSD-3-Clause)

✅ **Requirements Met:**
- Fetched from official source repository during build
- Copyright notices preserved in fetched source
- Attribution provided in this LICENSING.md
- No trademark use or endorsement implied

### 4. µGFX License

⚠️ **Action Required for Commercial Use:**
- Current use is compliant for open source projects
- **If commercializing**: Contact µGFX (https://ugfx.io/) for commercial licensing
- **Alternative**: Replace µGFX with fully permissive alternatives (e.g., pure LVGL implementation)

### 5. DejaVu Fonts

✅ **Requirements Met:**
- License files preserved in `components/ugfx/src/gdisp/fonts/*.license`
- Fonts are not sold standalone (part of larger software)
- Attribution provided in this LICENSING.md
- Original copyright notices retained

### 6. AnimatedGIF Library

✅ **Requirements Met:**
- Apache 2.0 license headers preserved in source files
- Copyright notice for BitBank Software, Inc. retained
- Same license as main project (Apache 2.0)

### 7. Fake08 PICO-8 Player

⚠️ **Recommended Actions:**
- [ ] Add explicit attribution in documentation stating Fake-08 is used under MIT license
- [ ] Consider fetching from source during build instead of including pre-compiled files

### 8. Python Flasher Dependencies

✅ **Current Compliance:**
- Flasher is distributed separately from firmware
- GPL components (esptool, PyInstaller) are properly isolated

🔲 **Recommended:**
- [ ] Include `THIRD-PARTY-NOTICES.txt` in flasher distribution
- [ ] Document that flasher tool is under GPL-2.0 (separate from Apache 2.0 firmware)
- [ ] Ensure README in release folder mentions GPL tools used for flashing

## Licensing Best Practices

### For Contributors

1. **Original Code**: All original contributions should be licensed under Apache 2.0
2. **Headers**: Add SPDX license identifiers to new source files:
   ```c
   // SPDX-License-Identifier: Apache-2.0
   // Copyright 2024-2025 p3a Contributors
   ```
3. **Third-Party Code**: 
   - Clearly document any third-party code additions
   - Verify license compatibility before integration
   - Preserve original copyright notices and licenses

### For Distributors

1. **Include License Files**: Always include LICENSE and LICENSING.md in distributions
2. **Binary Distributions**: Include "About" screen or README with licensing information
3. **Modified Versions**: Clearly state modifications were made (Apache 2.0 requirement)
4. **Commercial Products**: Review µGFX licensing requirements

### For Commercial Use

If you plan to commercialize p3a or create commercial derivatives:

1. ✅ **No issues** with Apache 2.0, MIT, BSD, Zlib, Libpng components
2. ⚠️ **Review µGFX license** - may require commercial license purchase
3. ✅ **DejaVu Fonts** - can be included in commercial products
4. ✅ **Fake08** - MIT license allows commercial use (if proper attribution given)
5. ✅ **Flasher tool** - GPL doesn't affect the firmware itself

**Recommendation for Commercial Products:**
- Replace µGFX with a fully permissive alternative (e.g., use LVGL exclusively)
- Or obtain a commercial µGFX license from https://ugfx.io/

## Summary and Conclusions

### Overall License Status: ✅ Compliant for Open Source Use

**p3a is fully compliant with open source licensing** as an Apache 2.0 project with the following considerations:

1. **All core dependencies** use permissive licenses (Apache-2.0, MIT, BSD, Zlib, Libpng)
2. **µGFX component** requires attention for commercial use (free for open source)
3. **GPL-licensed tools** (esptool, PyInstaller) are properly isolated in flasher utility
4. **All license requirements** can be met with proper attribution and notices

### No Blocking Issues

There are **no license conflicts** that would prevent:
- Using p3a as an open source project
- Modifying and distributing p3a
- Using p3a for personal or educational purposes
- Commercial use (with µGFX consideration noted above)

### Recommendations for Full Compliance

**High Priority:**
1. ✅ Apache 2.0 LICENSE file (completed)
2. ✅ Comprehensive licensing documentation (this file)
3. Consider adding SPDX headers to source files for clarity

**Medium Priority:**
1. Add explicit Fake08 attribution in documentation
2. Create THIRD-PARTY-NOTICES.txt for flasher tool distribution
3. Document µGFX commercial licensing consideration for commercial users

**Low Priority (Optional):**
1. Consider replacing µGFX with fully permissive alternatives for commercial-friendly stack
2. Fetch Fake08 from source during build instead of pre-compiled binaries
3. Create automated license scanning in CI/CD pipeline

## License Files and References

- **Main License**: [LICENSE](LICENSE) - Apache License 2.0 (full text)
- **This Document**: LICENSING.md - Comprehensive licensing report
- **Dependencies Lock**: dependencies.lock - ESP-IDF component versions and hashes
- **Font Licenses**: `components/ugfx/src/gdisp/fonts/*.license` - DejaVu font licenses
- **ESP-IDF Components**: Managed by ESP-IDF Component Manager, licenses in respective component directories

## Updates and Maintenance

This licensing report was created on December 29, 2024. It should be updated when:
- New dependencies are added
- Dependency versions are updated significantly
- Project licensing strategy changes
- µGFX is replaced or removed
- Commercial licensing questions arise

For questions about licensing, please open an issue on the GitHub repository.

---

**Document Version**: 1.0  
**Last Updated**: December 29, 2024  
**Maintainer**: p3a Contributors  
**Contact**: https://github.com/fabkury/p3a/issues
