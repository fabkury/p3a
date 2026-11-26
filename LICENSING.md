# p3a licensing

What p3a 2.x is licensed under, and the licence of everything it is built with or carries. For 1.x, see this file on the `main` branch.

## p3a itself

p3a is licensed under the **Apache License, Version 2.0** ([LICENSE](LICENSE), SPDX `Apache-2.0`). Copyright 2025-2026 p3a Contributors.

It allows commercial use, modification and distribution, grants patent rights from contributors, and asks that copyright and licence notices be kept.

## What the firmware is built with

### ESP-IDF

ESP-IDF v5.5.4, Apache-2.0, by Espressif Systems. It brings its own third-party parts (FreeRTOS, lwIP, mbedTLS, cJSON and others), each under a permissive licence listed in the IDF's own documentation.

### Components from the component registry

Declared in each component's `idf_component.yml`, pinned to exact versions, and recorded in `dependencies.lock`.

| Component | Version | Licence | What for |
|---|---|---|---|
| espressif/esp_hosted | 2.9.7 | Apache-2.0 | The link to the ESP32-C6 that does Wi-Fi |
| espressif/esp_wifi_remote | 1.2.2 | Apache-2.0 | The Wi-Fi API over that link |
| espressif/eppp_link | 1.1.5 | Apache-2.0 | Required by the above |
| espressif/wifi_remote_over_eppp | 0.3.2 | Apache-2.0 | Required by the above |
| espressif/esp_serial_slave_link | 1.1.2 | Apache-2.0 | Required by the link to the ESP32-C6 |
| espressif/mdns | 1.9.1 | Apache-2.0 | The device's name on the network |
| espressif/esp_tinyusb | 1.7.6~2 | Apache-2.0 | The SD card as a USB drive |
| espressif/tinyusb | 0.17.0~2 | MIT | The USB stack under it |
| espressif/libjpeg-turbo | 3.1.1~1 | IJG, BSD-3-Clause and Zlib (libjpeg-turbo's three) | JPEG decoding |
| espressif/zlib | 1.3.2 | Zlib | PNG decompression |
| espressif/esp_lcd_touch | 1.2.1 | Apache-2.0 | Touch |
| espressif/esp_lcd_touch_gt911 | 1.2.0~2 | Apache-2.0 | The GT911 touch controller |
| espressif/esp_codec_dev | 1.2.0 | Apache-2.0 | Required by the board package; unused |
| espressif/esp_lvgl_port | 2.8.0~1 | Apache-2.0 | Required by the board package; unused |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 | Build helpers |
| lvgl/lvgl | 9.4.0 | MIT | Required by the board package; p3a does not use LVGL |
| waveshare/esp32_p4_wifi6_touch_lcd_4b | 1.0.1 | Apache-2.0 | The board package |
| waveshare/esp_lcd_st7703 | 1.0.5 | Apache-2.0 | The panel's driver |

### Fetched at build time

| Library | Version | Licence | What for |
|---|---|---|---|
| libwebp | 1.4.0 | BSD-3-Clause | WebP decoding. Fetched from `chromium.googlesource.com/webm/libwebp` by `components/libwebp_decoder` |

### In the source tree

| What | Where | Licence | Notes |
|---|---|---|---|
| libpng 1.6.52 with the APNG read patch | `components/libpng/` | libpng licence | `components/libpng/LICENSE`. The patch is the official APNG one, under the same licence |
| AnimatedGIF | `components/animated_gif/` | Apache-2.0 | Copyright 2020 BitBank Software, Inc. (Larry Bank). Notices are in its source files |
| Everyday Ample (pixel font) | `components/p3a_ui/fonts/ui.p3afont` | CC BY 4.0 | By [VEXED](https://v3x3d.itch.io/). The glyphs are unchanged; only the container differs. Attribution is given here, in the font folder's README, and on the device's Settings page |
| ESP32-C6 firmware (`network_adapter.bin`) | `components/slave_ota/firmware/` | Apache-2.0 | Espressif's esp-hosted slave firmware, prebuilt |

## What is not part of the firmware

### The web flasher

`docs/web-flasher/esptool-bundle.js` is [esptool-js](https://github.com/espressif/esptool-js), Apache-2.0, by Espressif Systems.

### The flasher program

`host/p3a-flasher` is a separate program. Built into `p3a-flasher.exe`, it bundles:

| Package | Licence |
|---|---|
| esptool | GPL-2.0-or-later |
| pyserial | BSD-3-Clause |
| requests | Apache-2.0 |
| Pillow | HPND |
| PyInstaller (its bootloader) | GPL-2.0-or-later with an exception that permits bundling programs under other licences |

esptool is under the GPL. The flasher runs it as a library inside a separate executable that is distributed apart from the firmware and does not link with it, so the firmware's licence is unaffected. The flasher's own source is in this repository, and anyone can use esptool directly instead (`flash_command.txt` in every release).

### Tests

`tests/host/third_party/` holds [Unity](https://github.com/ThrowTheSwitch/Unity) (MIT) and [cJSON](https://github.com/DaveGamble/cJSON) (MIT), each with its licence file. They are compiled into the host tests only.

## Content the device shows

p3a displays artworks from Makapix Club, Giphy, Klipy, museums and the owner's own files. They are not part of p3a and each remains under its own terms: the artist's, the service's, or the museum's open-access policy. The device identifies its source on screen where the source's terms ask for it.

## Summary

Every part of the firmware is under a permissive licence compatible with Apache-2.0 (Apache-2.0, MIT, BSD, Zlib, libpng, IJG), plus one font under CC BY 4.0, whose attribution requirement is met above. No part of the firmware is under a copyleft licence. The only GPL software involved is in the optional flasher program, which is separate from the firmware.

This page is updated when a dependency is added, removed, or changes version (`dependencies.lock` is the record of versions).
