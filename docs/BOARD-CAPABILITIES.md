# Board Capabilities Reference

Hardware capabilities of the ESP32-P4-WIFI6-Touch-LCD-4B board used by p3a.

- **Board**: [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)
- **Wiki**: [ESP32-P4-WIFI6-Touch-LCD-4B Wiki](http://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B)
- **SoC**: ESP32-P4NRW32 (RISC-V dual-core HP @ 360 MHz + single-core LP @ 40 MHz)
- **Memory**: 32 MB PSRAM (in-package), 32 MB NOR Flash (on-board)
- **Display**: 4" 720×720 IPS, MIPI-DSI, capacitive 5-point touch (GT911)

---

## Table of Contents

- [Audio Subsystem](#audio-subsystem)
- [Hardware Accelerators](#hardware-accelerators)
  - [JPEG Codec](#jpeg-codec)
  - [Pixel-Processing Accelerator (PPA)](#pixel-processing-accelerator-ppa)
  - [H.264 Encoder](#h264-encoder)
  - [2D-DMA Controller](#2d-dma-controller)
  - [Image Signal Processor (ISP)](#image-signal-processor-isp)
  - [Cryptographic Accelerators](#cryptographic-accelerators)
- [Display Interface (MIPI-DSI)](#display-interface-mipi-dsi)
- [Connectivity and Peripherals](#connectivity-and-peripherals)
- [p3a Usage Summary](#p3a-usage-summary)

---

## Audio Subsystem

The board has a complete audio output path wired on-board, including a speaker pre-connected to the speaker header.

### Audio Signal Chain

```
ESP32-P4  ──I2S──>  ES8311 (DAC)  ──analog──>  NS4150B (Class-D Amp)  ──>  8Ω 2W Speaker
           ──I2C──>  ES8311 (config)
```

### Audio Hardware

| Component | Role | Key Specs |
|-----------|------|-----------|
| **ES8311** | Mono audio codec (Everest Semiconductor) | 24-bit DAC, 8–96 kHz sample rate, 110 dB SNR, 1.8–3.3V |
| **NS4150B** | 3W mono Class-D power amplifier (Nsiway) | Up to 2.8W @ 5V/4Ω, 88% efficiency, filterless |
| **Speaker** | 8Ω 2W (MX1.25 2P connector) | Pre-connected on the board |
| **ES7210** | Echo cancellation ADC (microphone input) | For recording, not playback |

### Audio Pin Mapping

| Signal | GPIO | Notes |
|--------|------|-------|
| `BSP_I2S_SCLK` | GPIO12 | I2S bit clock |
| `BSP_I2S_MCLK` | GPIO13 | I2S master clock |
| `BSP_I2S_LCLK` | GPIO10 | I2S word select (LR clock) |
| `BSP_I2S_DOUT` | GPIO9 | I2S data out (ESP32 → codec) |
| `BSP_I2S_DSIN` | GPIO11 | I2S data in (codec → ESP32) |
| `BSP_POWER_AMP_IO` | GPIO53 | Power amplifier enable (active HIGH) |

### Audio Software Stack

The Waveshare BSP (`waveshare/esp32_p4_wifi6_touch_lcd_4b`) provides ready-to-use functions. It transitively pulls in `espressif/esp_codec_dev` v1.2.0.

**BSP functions:**

- `bsp_audio_init(i2s_config)` — Initialize I2S (default: 22050 Hz, mono, 16-bit)
- `bsp_audio_codec_speaker_init()` — Initialize ES8311 in DAC mode, configure PA GPIO, return `esp_codec_dev_handle_t`
- `bsp_audio_codec_microphone_init()` — Initialize ES7210 for recording

**Playback API (from `esp_codec_dev`):**

```c
esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
esp_codec_dev_set_out_vol(speaker, 60);

esp_codec_dev_sample_info_t fs = {
    .sample_rate = 22050,
    .channel = 1,
    .bits_per_sample = 16,
};
esp_codec_dev_open(speaker, &fs);
esp_codec_dev_write(speaker, pcm_buffer, pcm_size);
esp_codec_dev_close(speaker);
```

### ES8311 DAC Format Support

| Parameter | Supported Values |
|-----------|-----------------|
| Sample Rate | 8, 11.025, 16, 22.05, 32, 44.1, 48, 64, 88.2, 96 kHz |
| Bit Depth | 16, 24, 32 bit |
| Channels | Mono (single DAC) |
| Audio Format | I2S standard, left-justified, right-justified, PCM/DSP |

### Audio Content Codec Options

The ES8311 accepts only raw PCM data; compressed audio must be decoded first.

| Format | Decoder | Notes |
|--------|---------|-------|
| Raw WAV (PCM) | None needed | Simplest; store .wav at the right sample rate |
| MP3 | `espressif/esp_audio_codec` or `libhelix-mp3` | Available as ESP-IDF components |
| AAC | `espressif/esp_audio_codec` | Available as ESP-IDF component |
| OPUS | `espressif/esp_audio_codec` | Good for voice/notification sounds |
| FLAC | `espressif/esp_audio_codec` | Lossless, larger files |
| Tone generation | Custom code | Programmatic sine waves / beeps |

---

## Hardware Accelerators

### JPEG Codec

A dedicated JPEG baseline engine. DCT, quantization, and Huffman coding run entirely in hardware; the CPU only parses headers.

| Capability | Details |
|-----------|---------|
| **Decode performance** | 480×320: 571 fps / 720×1280: 109 fps / 1080×1920: 48 fps |
| **Encode performance** | 720×1280 RGB565→YUV420: 88 fps / 1080p RGB888→YUV422: 26 fps |
| **Decode input formats** | YUV444, YUV422, YUV420, GRAY |
| **Decode output formats** | RGB888, RGB565, GRAY, YUV444, YUV422, YUV420 |
| **Encode input formats** | RGB888, RGB565, GRAY, YUV422, YUV444, YUV420 |
| **Max still resolution** | Up to 4K |
| **Limitation** | Cannot encode and decode simultaneously (shared engine) |
| **ESP-IDF driver** | `esp_driver_jpeg` (`driver/jpeg_decode.h`, `driver/jpeg_encode.h`) |

**ESP-IDF API:**
- `jpeg_new_decoder_engine()` / `jpeg_decoder_process()` / `jpeg_del_decoder_engine()`
- `jpeg_new_encoder_engine()` / `jpeg_encoder_process()` / `jpeg_del_encoder_engine()`
- `jpeg_decoder_get_info()` — Parse JPEG header without decoding
- `jpeg_alloc_decoder_mem()` / `jpeg_alloc_encoder_mem()` — Aligned memory allocation helpers

### Pixel-Processing Accelerator (PPA)

A dedicated 2D image processing engine for pixel-level transformations with zero CPU involvement.

#### SRM (Scale-Rotate-Mirror)

| Parameter | Details |
|-----------|---------|
| Operations | Scaling, rotation (0°/90°/180°/270° CCW), horizontal/vertical mirroring |
| Scaling precision | 1/16 step (4-bit integer + 8-bit fractional) |
| Input color modes | ARGB8888, RGB888, RGB565, YUV420, YUV444 (input only), YUV422, GRAY8 |
| Output color modes | ARGB8888, RGB888, RGB565, YUV420, YUV422, GRAY8 |
| Algorithm | Bilinear interpolation |

#### Blend

| Parameter | Details |
|-----------|---------|
| Operation | Alpha blending of foreground + background layers |
| FG input modes | ARGB8888, RGB888, RGB565, A8, A4 |
| BG input modes | ARGB8888, RGB888, RGB565, YUV420, YUV422, GRAY8 |
| Output modes | ARGB8888, RGB888, RGB565, YUV420, YUV422, GRAY8 |
| Features | Color keying (chroma key), configurable alpha update modes |

#### Fill

| Parameter | Details |
|-----------|---------|
| Operation | Fill a rectangular region with a constant color |
| Color modes | ARGB8888, RGB888, RGB565, YUV422, GRAY8 |

**ESP-IDF driver:** `esp_driver_ppa` (`driver/ppa.h`)

**API:**
- `ppa_register_client()` / `ppa_unregister_client()`
- `ppa_do_scale_rotate_mirror()` / `ppa_do_blend()` / `ppa_do_fill()`
- Supports blocking and non-blocking transaction modes
- Thread-safe across clients and tasks

**Performance note:** PPA throughput is proportional to block size and highly dependent on PSRAM bandwidth. When many peripherals access PSRAM simultaneously, performance degrades.

### H.264 Encoder

A baseline H.264 video encoder for real-time compression. Encode-only (no hardware H.264 decoder exists).

| Capability | Details |
|-----------|---------|
| Max performance | 1920×1080 @ 30 fps |
| Input format | YUV420 progressive |
| Frame types | I-frame, P-frame |
| Features | GOP mode, dual-stream mode, ROI (up to 8 regions), deblocking filter, motion vector output, P-skip |
| Prediction | All 9 intra 4×4 modes, all 4 intra 16×16 modes, all inter partition modes |
| Rate control | Fixed QP or macroblock-level (QP range 0–51) |
| Coding | CAVLC (Context Adaptive Variable Length Coding) |
| ESP-IDF component | `espressif/esp_h264` |

### 2D-DMA Controller

A specialized DMA engine for two-dimensional image data, distinct from the general-purpose GDMA.

| Capability | Details |
|-----------|---------|
| Purpose | Memory-to-memory 2D block transfers, macroblock reordering, color space conversion |
| Supported peripherals | JPEG codec, PPA |
| CSC | Color space conversion during transfer (e.g., YUV ↔ RGB) |
| Architecture | AXI bus, unaligned starting addresses, configurable channel priority |
| Channels | 3 TX (memory→peripheral), 2 RX (peripheral→memory) |
| Integration | Also used by LVGL display driver to optimize frame buffer transfers via MIPI-DSI |

### Image Signal Processor (ISP)

A full camera image processing pipeline (designed for MIPI-CSI camera input).

| Capability | Details |
|-----------|---------|
| Max resolution | 1920×1080 |
| Input sources | MIPI-CSI, DVP camera, system memory via DMA |
| Input formats | RAW8, RAW10, RAW12 |
| Output formats | RAW8, RGB888, RGB565, YUV422, YUV420 |
| Pipeline stages | Bayer filter, black level correction, lens shading correction, demosaic, CCM, gamma correction, sharpening, contrast/hue/saturation/luminance, auto-focus (AF), auto-white balance (AWB), auto-exposure (AE), histogram |

### Cryptographic Accelerators

| Accelerator | Key Specs |
|------------|-----------|
| **AES** | AES-128/256, ECB/CBC/OFB/CTR/CFB/GCM modes, DMA-capable |
| **SHA** | SHA-1/224/256/384/512 and variants, DMA-capable |
| **RSA** | Up to 4096-bit modular exponentiation/multiplication |
| **ECC** | P-192 and P-256 curves |
| **ECDSA** | P-192/P-256 with SHA-224/256 |
| **HMAC** | HMAC-SHA-256 with eFuse-protected keys |
| **DSA** | RSA digital signatures up to 4096-bit |
| **XTS-AES** | Flash and PSRAM transparent encryption/decryption |
| **TRNG** | True random number generator |

---

## Display Interface (MIPI-DSI)

| Parameter | Details |
|-----------|---------|
| Standard | MIPI DSI, D-PHY v1.1 |
| Bandwidth | 2-lane × 1.5 Gbps (3 Gbps total) |
| Input formats | RGB888, RGB666, RGB565, YUV422 |
| Output formats | RGB888, RGB666, RGB565 |
| Mode | Video mode for streaming; also supports fixed image patterns |
| Display | 4" 720×720 IPS, GT911 capacitive touch (5-point) |
| Backlight | GPIO26, LEDC-controlled brightness |
| Brightness | 400 cd/m², contrast 1200:1, viewing angle 170° |

---

## Connectivity and Peripherals

| Peripheral | Details |
|-----------|---------|
| **USB 2.0 HS OTG** | 480 Mbps, Type-A connector |
| **USB-to-UART** | Type-C, for flashing and debug serial |
| **SD/MMC (SDIO 3.0)** | 4-wire, default 20 MHz / high-speed 40 MHz |
| **Wi-Fi 6** | Via ESP32-C6-MINI co-processor over SDIO (2.4 GHz) |
| **Bluetooth 5 (LE)** | Via ESP32-C6-MINI co-processor |
| **I2C** | SCL: GPIO8, SDA: GPIO7 (ES8311 codec, GT911 touch) |
| **I2S** | 3 controllers available; one used for audio codec |
| **Ethernet** | IP101 PHY, 10/100M RJ45 (only on ESP32-P4-86-Panel-ETH-2RO variant) |
| **MIPI-CSI** | 2-lane camera interface (15-pin connector on this board variant) |
| **Microphones** | Dual onboard SMD microphones with ES7210 echo cancellation |
| **Speaker** | MX1.25 2P connector, 8Ω 2W (pre-connected) |
| **RTC Battery** | Header for rechargeable RTC battery |
| **Voice Activity Detection** | Analog-domain, ultra-low-power speech detection |

---

## p3a Usage Summary

| Capability | Used by p3a? | Notes |
|-----------|:---:|-------|
| **JPEG HW Codec** | Yes (decode) | `animation_decoder` decodes JPEG artwork via hardware |
| **PPA** | No | Could accelerate upscaling, rotation, alpha blending |
| **H.264 Encoder** | No | Encode-only; not applicable to playback |
| **2D-DMA** | Indirectly | Serves JPEG decoder; may assist display path |
| **ISP** | No | Camera pipeline; not needed without camera |
| **MIPI-DSI** | Yes | Drives the 720×720 display |
| **Crypto (AES/SHA/RSA)** | Yes (via TLS) | TLS for MQTT, HTTPS, OTA |
| **USB 2.0 HS** | Yes | USB MSC for file transfer |
| **SD/MMC** | Yes | Artwork storage on SD card |
| **I2S + Audio** | No | Speaker playback available but not yet integrated |
| **Wi-Fi 6 (ESP32-C6)** | Yes | Makapix, Giphy, OTA |
| **VAD** | No | Could enable low-power voice wake |

---

*Sources: [ESP32-P4 Datasheet v1.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html), [ESP-IDF v5.5.3 Programming Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/), [Waveshare Wiki](http://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B), [esp_codec_dev v1.5.4](https://components.espressif.com/components/espressif/esp_codec_dev)*
