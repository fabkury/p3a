# Performance Debugging Instructions for p3a

## Overview

The codebase has been instrumented to measure the duration of each main step in the graphics pipeline. The instrumentation distinguishes between "target animations" (the slow-playing sonic_animation) and all other animations.

## What Was Instrumented

### 1. Frame Rendering Pipeline (`main/animation_player_render.c`)
- **render_next_frame()**: Measures total frame rendering time and breaks it down into:
  - WebP decode time
  - Upscaling time
  - Total pipeline time
  - Target frame delay (from animation file)

### 2. WebP Decoder (`components/animation_decoder/webp_animation_decoder.c`)
- **animation_decoder_decode_next_rgb()**: Measures:
  - WebP library decode time (libwebp `WebPAnimDecoderGetNext`)
  - RGBA-to-RGB conversion time (for opaque animations)
  - Alpha blending time (for transparent animations)
  - Pixel count and alpha presence

### 3. Animation Detection
- Automatically detects when the target animation is loaded
- Target: `/sdcard/vault/a7/55/ae/e7fbb22e-3c16-46bd-b488-53ab8dc4c524.webp`
- Logs are tagged with "TARGET" or "OTHER" to distinguish datasets

### 4. Sampling Strategy
- Logs every 10th frame to minimize HTTP overhead
- Uses non-blocking HTTP requests to minimize impact on performance

## Setup Instructions

### Step 1: Find Your Laptop's Local IP Address

**Windows:**
```powershell
ipconfig
```
Look for "IPv4 Address" under your active network adapter (e.g., `192.168.1.100`)

**macOS/Linux:**
```bash
ifconfig
# or
ip addr show
```
Look for your local network interface's inet address (e.g., `192.168.1.100`)

### Step 2: Update Debug Configuration

Edit `components/debug_http_log/debug_http_log.h` and change line 25:

```c
#define DEBUG_LOG_SERVER_IP "192.168.20.61"  // CHANGE THIS!
```

Replace `"192.168.20.61"` with your laptop's actual IP address from Step 1.

**IMPORTANT:** The ESP32-P4 device and your laptop must be on the same local network!

**NOTE:** You already have this set to `192.168.20.61` - verify this is correct for your network.

### Step 3: Build and Flash the Firmware

```powershell
# In PowerShell, from the project root directory:
C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1
idf.py build
idf.py -p COM5 flash
```

### Step 4: Monitor the Device (Optional)

```powershell
idf.py -p COM5 monitor
```

Press `Ctrl+]` to exit the monitor.

### Step 5: Run the Test

1. Make sure your laptop and ESP32 are on the same network
2. Start the ndjson ingest server (should already be running)
3. Navigate the device to play the sonic_animation:
   - `/sdcard/vault/a7/55/ae/e7fbb22e-3c16-46bd-b488-53ab8dc4c524.webp`
4. Let it play for at least 20-30 frames (2-3 seconds)
5. Navigate to other animations and let them play for comparison
6. The device will send logs to your laptop automatically

## Hypotheses Being Tested

**Hypothesis A: WebP Decoding Bottleneck**
- Measured by: `webp_decode_us` in logs
- Expected: Sonic animation has longer decode times than others

**Hypothesis B: Alpha Blending Overhead**  
- Measured by: `alpha_blend_us` in logs (only present if animation has transparency)
- Expected: Transparent animations show significant blending overhead

**Hypothesis C: Upscaling Bottleneck**
- Measured by: `upscale_us` in logs
- Expected: Larger source frames take longer to upscale

**Hypothesis D: Frame Timing Bug**
- Measured by: comparing `total_us` with `target_ms`
- Expected: If total_us > target_ms * 1000, frame is late

**Hypothesis E: Cumulative Pipeline Overhead**
- Measured by: `total_us` = decode + upscale + overhead
- Expected: Sum of all stages exceeds target frame time

## Expected Log Format

Logs will appear in `.cursor\debug.log` as NDJSON entries. Example entries:

### Target Animation Frame Loaded:
```json
{"timestamp":1234567890,"location":"animation_player_render.c:490","message":"target_animation_loaded","sessionId":"debug-session","hypothesisId":"TARGET","data":{"width":128}}
```

### Frame Rendering Complete:
```json
{"timestamp":1234567890,"location":"animation_player_render.c:285","message":"frame_render_complete","sessionId":"debug-session","hypothesisId":"TARGET","data":{"decode_us":15000,"upscale_us":8000,"total_us":23500,"target_ms":33}}
```

### WebP Decode (Opaque):
```json
{"timestamp":1234567890,"location":"webp_animation_decoder.c:340","message":"webp_decode_opaque","sessionId":"debug-session","hypothesisId":"A","data":{"webp_decode_us":14000,"rgba_to_rgb_us":500,"pixel_count":16384,"has_alpha":0}}
```

### WebP Decode (With Alpha):
```json
{"timestamp":1234567890,"location":"webp_animation_decoder.c:366","message":"webp_decode_alpha","sessionId":"debug-session","hypothesisId":"B","data":{"webp_decode_us":14000,"alpha_blend_us":3000,"pixel_count":16384,"has_alpha":1}}
```

## Troubleshooting

### Logs Not Appearing

1. **Check network connectivity:**
   - Verify ESP32 and laptop are on same network
   - Ping the laptop IP from another device
   - Check firewall settings (Windows Firewall may block incoming connections)

2. **Check the ingest server:**
   - Is the ndjson ingest server running?
   - Check the log in Cursor's debug mode

3. **Check ESP32 console output:**
   ```powershell
   idf.py -p COM5 monitor
   ```
   Look for HTTP errors or network issues

### Performance Seems Different

- The instrumentation adds ~1-2ms overhead per frame due to HTTP logging
- This is intentional and acceptable for debugging
- Focus on *relative* differences between TARGET and OTHER animations

### Build Errors

If you get build errors about missing dependencies:
```powershell
idf.py reconfigure
idf.py build
```

## Next Steps After Data Collection

Once you have collected logs:
1. Type "done" or "proceed" in Cursor
2. I will analyze the logs and identify the root cause
3. I will implement targeted optimizations based on the evidence
4. We will verify the fix with a second round of measurements

