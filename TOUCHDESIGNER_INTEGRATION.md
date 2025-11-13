# TouchDesigner to P3A Display Integration - Implementation Plan

## Overview

This document outlines three architectural approaches for sending images from TouchDesigner to the P3A pixel art display device. Each option represents different trade-offs between implementation complexity, performance, and real-time capabilities.

---

## TouchDesigner Architecture & Capabilities

### Understanding TouchDesigner's Texture System

TouchDesigner uses **TOPs (Texture Operators)** for all image/video processing:
- **TOP Class**: Represents texture/image operators
- **numpyArray()**: Method to export TOP pixel data as NumPy arrays
- **Resolution**: TOPs can be any resolution, require resizing to 720×720 for P3A
- **Color Space**: TouchDesigner uses normalized floats (0.0-1.0 RGB), need conversion to uint8

### Key TouchDesigner Components for Network I/O

#### 1. Web Client DAT (HTTP Requests)
- **Purpose**: Send HTTP requests (GET, POST, PUT, DELETE)
- **File Upload Support**: Can send multipart/form-data or raw binary
- **Callbacks**: `onDone`, `onError` for async response handling
- **Documentation**: https://derivative.ca/UserGuide/Web_Client_DAT

**Advantages**:
- Native TouchDesigner component (no external libraries)
- Visual wiring in network
- Built-in error handling callbacks

**Limitations**:
- Stateless (one request at a time)
- No persistent connection
- Callback-based (requires DAT callbacks)

#### 2. WebSocket DAT (Real-Time Streaming)
- **Purpose**: Bi-directional WebSocket communication
- **Binary Support**: Can send/receive binary frames
- **Persistent Connection**: Maintains open connection for streaming
- **Documentation**: https://derivative.ca/UserGuide/WebSocket_DAT

**Advantages**:
- Low latency for continuous streaming
- Bi-directional (P3A can send status back)
- Event-driven with callbacks

**Limitations**:
- More complex setup than HTTP
- Requires connection management
- Binary protocol needs manual framing

#### 3. Python Script DAT (Custom Code)
- **Purpose**: Execute Python code with external libraries
- **Libraries**: Can use `requests`, `websockets`, `PIL/Pillow`
- **Execution**: Run via textport or callbacks

**CRITICAL LIMITATION - Textport Execution**:
- TouchDesigner textport **CANNOT** execute multi-line indented code
- Must save scripts to files and execute via: `exec(open(r'scripts/filename.py').read())`
- Do NOT provide inline code blocks with loops, functions, or if statements

### Modern TouchDesigner Python Features (v2023+)

#### TDI Library (TouchDesigner Interface Library)
- **Purpose**: Modern Python interface with better type hints
- **Documentation**: https://docs.derivative.ca/TDI_Library
- **Use Case**: Improved IDE autocomplete and type checking
- **When to Use**: For complex Python scripts with better tooling support

#### Thread Manager
- **Purpose**: Simplified multi-threading for non-blocking operations
- **Documentation**: https://docs.derivative.ca/Thread_Manager
- **Use Case**: Perfect for network uploads without blocking TouchDesigner UI
- **Key Benefit**: Handles thread lifecycle, callbacks, and synchronization

**Example Use Case for P3A Integration**:
```python
# Use Thread Manager for async HTTP uploads
# Prevents blocking TouchDesigner's main thread during image upload
# Allows continuous rendering while network transfer happens in background
```

#### Python Environment Manager (tdPyEnvManager)
- **Purpose**: Manage external Python packages
- **Documentation**: https://docs.derivative.ca/Palette:tdPyEnvManager
- **Use Case**: Install `requests`, `pillow`, `numpy` dependencies
- **Installation**: Via Palette browser (drag-and-drop component)

### TouchDesigner Parameter Access Patterns

```python
# Accessing operator parameters (verified syntax):
op('web_client1').par.url.val           # Get parameter value
op('web_client1').par.url = 'http://...' # Set parameter value
op('web_client1').par.request.pulse()    # Trigger pulse parameter

# Common operators for P3A integration:
op('render_out')        # TOP operator with rendered output
op('web_client1')       # Web Client DAT for HTTP uploads
op('websocket1')        # WebSocket DAT for streaming
op('upload_script')     # Script DAT for Python upload logic
```

### TouchDesigner Execution Model

**Frame-Based Execution**:
- TouchDesigner runs at specific frame rate (e.g., 60 fps)
- All operators update once per frame (unless cooked on-demand)
- Network operations should NOT block frame updates

**Best Practice for P3A Integration**:
1. Use **Timer CHOP** to trigger uploads at specific intervals (e.g., 1 fps, 5 fps)
2. Use **Thread Manager** or async callbacks to avoid blocking
3. Monitor upload success/failure via callback DATs
4. Cache the last successful upload to retry on failure

---

## Current P3A Architecture Analysis

### Display Specifications
- **Resolution**: 720×720 pixels (4" square IPS display)
- **Interface**: MIPI-DSI
- **Color Format**: RGB565 or RGB888
- **Hardware**: ESP32-P4 dual-core, ESP32-C6 WiFi coprocessor
- **Storage**: microSD card (FAT32)

### Existing Animation Pipeline
1. **File Loading**: `animation_player.c:1392` - `load_animation_file_from_sd()`
   - Uses `fopen()`/`fread()` to load entire file into RAM
   - Typical file size: 50KB-2MB (compressed)

2. **Format Decoding**: Modular decoder interface (`animation_decoder.h`)
   - Supports: WebP, GIF, PNG, JPEG
   - Input: `const uint8_t *data, size_t size` (raw memory buffer)
   - Output: RGBA frames decoded to native frame buffers

3. **Display Rendering**:
   - Double-buffered frame swapping
   - Hardware upscaling (if source < 720×720)
   - DMA transfer to LCD

### Existing Network Infrastructure
- **HTTP Server**: Port 80, ESP-IDF HTTP server (`components/http_api/`)
- **mDNS**: `p3a.local` hostname resolution
- **REST Endpoints**:
  - `GET /` - Web UI remote control
  - `GET /status` - Device status JSON
  - `POST /action/swap_next` - Navigate animations
  - `POST /action/swap_back` - Navigate animations
  - `PUT /config` - Configuration management (max 32KB JSON)

### Memory Constraints
- **Free Heap**: Varies, typically 200KB-2MB available
- **DMA Buffers**: Pre-allocated for LCD frame buffers
- **SD Card Buffer**: 4KB chunks for file I/O

---

## Option 1: HTTP POST Upload (SD Card Persistence)

### Architecture

```
TouchDesigner                    P3A Device
┌──────────────┐                ┌─────────────────────┐
│ TOP Network  │                │                     │
│ (Web Client) │   HTTP POST    │  HTTP API Server    │
│              ├───────────────►│  (Port 80)          │
│  - Encode    │  multipart/    │                     │
│    image     │  form-data     │  ┌───────────────┐  │
│  - JPEG/PNG/ │                │  │ POST /upload/ │  │
│    WebP      │                │  │ image         │  │
└──────────────┘                │  └───────┬───────┘  │
                                │          │          │
                                │          ▼          │
                                │  ┌──────────────┐   │
                                │  │ Save to SD   │   │
                                │  │ /sdcard/     │   │
                                │  │ animations/  │   │
                                │  │ td_live.png  │   │
                                │  └──────┬───────┘   │
                                │         │           │
                                │         ▼           │
                                │  ┌──────────────┐   │
                                │  │ Call         │   │
                                │  │ animation_   │   │
                                │  │ player_      │   │
                                │  │ load_asset() │   │
                                │  └──────┬───────┘   │
                                │         │           │
                                │         ▼           │
                                │  ┌──────────────┐   │
                                │  │ Decode &     │   │
                                │  │ Display      │   │
                                │  └──────────────┘   │
                                └─────────────────────┘
```

### Implementation Details

#### ESP32 Side Changes

**1. New HTTP Endpoint** (`components/http_api/http_api.c`)
```c
POST /upload/image
Content-Type: multipart/form-data or application/octet-stream

Request Headers:
  - X-Image-Format: "png" | "jpeg" | "webp" (optional, can detect from magic bytes)
  - Content-Length: <file_size>

Response:
{
  "ok": true,
  "data": {
    "saved_path": "/sdcard/animations/td_live.png",
    "file_size": 123456,
    "display_updated": true
  }
}
```

**2. File Handling Logic**
- Receive chunked upload (4KB chunks via `httpd_req_recv()`)
- Write directly to SD card using `fwrite()` to avoid full RAM buffering
- Support maximum file size: 5MB (configurable via Kconfig)
- Atomic write: Save to temp file first, then rename on success

**3. Integration Hook**
- After successful SD write, call `animation_player_load_asset(filepath)`
- This reuses 100% of existing animation player infrastructure
- Automatic format detection based on file extension or magic bytes

#### TouchDesigner Side

**Method A: Python Script with External Libraries (Recommended for Flexibility)**

Save this script as `scripts/p3a_upload_http.py`:

```python
"""
P3A HTTP Upload Script for TouchDesigner
Sends TOP render output to P3A display via HTTP POST multipart upload

Dependencies: requests, pillow, numpy (install via tdPyEnvManager)
Usage: exec(open(r'scripts/p3a_upload_http.py').read())
"""
import requests
import numpy as np
from PIL import Image
import io

def upload_to_p3a(top_path='render_out', ip_address='p3a.local', format='PNG', quality=85):
    """
    Upload current frame from TOP to P3A display

    Args:
        top_path: Path to TOP operator (relative or absolute)
        ip_address: P3A device IP or hostname (default: 'p3a.local')
        format: Image format - 'PNG', 'JPEG', or 'WEBP' (default: 'PNG')
        quality: JPEG/WebP quality 0-100 (default: 85)

    Returns:
        bool: True if upload successful, False otherwise
    """
    try:
        # Get TOP operator
        top_op = op(top_path)
        if top_op is None:
            print(f"ERROR: TOP '{top_path}' not found")
            return False

        # Get image data from TOP (returns normalized float array)
        img_array = top_op.numpyArray(delayed=False)

        # Check if array is valid
        if img_array is None or img_array.size == 0:
            print(f"ERROR: TOP '{top_path}' has no image data")
            return False

        # Convert from float (0.0-1.0) to uint8 (0-255)
        # TouchDesigner uses bottom-left origin, flip vertically for standard image
        img_array = np.flipud(img_array)
        img_uint8 = (img_array * 255).astype(np.uint8)

        # Handle different channel counts (RGB vs RGBA)
        if img_uint8.shape[2] == 4:
            # RGBA - convert to RGB (P3A doesn't support transparency in upload)
            img = Image.fromarray(img_uint8[:, :, :3], mode='RGB')
        else:
            img = Image.fromarray(img_uint8, mode='RGB')

        # Resize to 720x720 if not already (P3A native resolution)
        if img.size != (720, 720):
            img = img.resize((720, 720), Image.LANCZOS)
            print(f"Resized from {top_op.width}x{top_op.height} to 720x720")

        # Encode to desired format
        buffer = io.BytesIO()
        if format.upper() == 'PNG':
            img.save(buffer, format='PNG', compress_level=6)
            content_type = 'image/png'
            filename = 'td_frame.png'
        elif format.upper() == 'JPEG':
            img.save(buffer, format='JPEG', quality=quality, optimize=True)
            content_type = 'image/jpeg'
            filename = 'td_frame.jpg'
        elif format.upper() == 'WEBP':
            img.save(buffer, format='WEBP', quality=quality)
            content_type = 'image/webp'
            filename = 'td_frame.webp'
        else:
            print(f"ERROR: Unsupported format '{format}'. Use PNG, JPEG, or WEBP")
            return False

        file_size = buffer.tell()
        buffer.seek(0)

        # HTTP POST upload (multipart/form-data)
        url = f'http://{ip_address}/upload/image'
        files = {'image': (filename, buffer, content_type)}

        print(f"Uploading {file_size} bytes to {url}...")
        response = requests.post(url, files=files, timeout=5)

        if response.ok:
            result = response.json()
            print(f"✓ Upload successful: {result}")
            return True
        else:
            print(f"✗ Upload failed: HTTP {response.status_code}")
            print(f"  Response: {response.text}")
            return False

    except Exception as e:
        print(f"✗ Upload error: {type(e).__name__}: {e}")
        return False

# Run upload (modify these parameters as needed)
upload_to_p3a(
    top_path='render_out',      # Your render TOP name
    ip_address='p3a.local',      # P3A IP or hostname
    format='JPEG',               # PNG, JPEG, or WEBP
    quality=85                   # For JPEG/WebP only
)
```

**Setup in TouchDesigner**:

1. **Install Dependencies** (one-time setup):
   - Open Palette (drag-and-drop `tdPyEnvManager` component)
   - Install packages: `requests`, `pillow`, `numpy`

2. **Create Timer CHOP** for periodic uploads:
   - Add Timer CHOP to network
   - Set `Rate`: 2 (for 2 fps uploads, adjust as needed)
   - Set `Length`: 1000000 (long duration)
   - Set `Mode`: Timer

3. **Create Execute DAT** for callbacks:
   - Add Execute DAT
   - Set `Trigger`: Timer CHOP from step 2
   - In `onValueChange` or `onPulse` callback:
     ```python
     def onValueChange(channel, sampleIndex, val, prev):
         # Trigger upload on timer pulse
         exec(open(r'scripts/p3a_upload_http.py').read())
     ```

4. **Test Upload** (run in textport):
   ```python
   exec(open(r'scripts/p3a_upload_http.py').read())
   ```

**Method B: Native Web Client DAT (No External Dependencies)**

For users who cannot install Python packages, use TouchDesigner's built-in Web Client DAT:

1. **Create TOP to PNG DAT**:
   - Add `TOP to PNG` DAT
   - Set `TOP`: Your render output
   - Set `Resolution`: 720 720
   - This converts TOP to base64-encoded PNG

2. **Create Web Client DAT**:
   - Add Web Client DAT
   - Set `URL`: `http://p3a.local/upload/image`
   - Set `Method`: POST
   - Set `Headers`: `Content-Type: multipart/form-data`
   - Set `Data`: Reference the TOP to PNG DAT output
   - Set `Request` parameter to pulse for upload

3. **Create Execute DAT** for callbacks:
   ```python
   def onDone(dat):
       print(f"Upload complete: {dat.result}")

   def onError(dat, errorMessage):
       print(f"Upload failed: {errorMessage}")
   ```

**Method C: Thread Manager (Non-Blocking, Advanced)**

For continuous streaming without blocking TouchDesigner's main thread:

```python
# Save as scripts/p3a_upload_threaded.py
# Uses TouchDesigner Thread Manager for async uploads

def threaded_upload(top_path, ip_address='p3a.local'):
    """Upload in background thread using Thread Manager"""
    # Import inside function (thread-safe)
    import requests
    import numpy as np
    from PIL import Image
    import io

    top_op = op(top_path)
    img_array = top_op.numpyArray(delayed=False)
    # ... (same encoding logic as Method A)

    # Upload happens in background thread
    response = requests.post(url, files=files, timeout=5)
    return response.ok

# Use Thread Manager to run upload asynchronously
run_async(threaded_upload, 'render_out', 'p3a.local')
```

### Performance Characteristics

**Latency Breakdown** (720×720 PNG, ~500KB file):
- TouchDesigner encode: ~10-30ms
- Network transfer (WiFi 6, same LAN): ~20-50ms
- SD card write: ~50-200ms (depends on card speed, class 10 recommended)
- File load to RAM: ~20-50ms
- Decode + display: ~30-100ms (depends on format)
- **Total: ~130-430ms per frame** → **2-7 fps max**

**Optimizations**:
- Use JPEG with quality=70 for smaller files (~100KB) → 100-300ms total (~3-10 fps)
- Use WebP for better compression than PNG
- Use fast SD cards (UHS-I, A1 rated)

### Pros
✅ **Simple implementation** (~200 lines of C code)
✅ **Reuses existing animation player** (zero changes to rendering pipeline)
✅ **Persistent images** (survives reboot, can be part of rotation)
✅ **Reliable** (HTTP retries, standard protocol)
✅ **Any format supported** (PNG/JPEG/WebP/GIF)
✅ **TouchDesigner has native HTTP support** (Web Client DAT)
✅ **No new dependencies** (uses existing ESP-IDF HTTP server)

### Cons
❌ **SD card write latency** (50-200ms bottleneck)
❌ **Limited frame rate** (2-7 fps practical max)
❌ **SD card wear** (finite write cycles, ~10K-100K depending on card)
❌ **No true real-time streaming** (not suitable for >10 fps live visuals)

### Best Use Cases
- **Periodic image updates** (every 1-5 seconds)
- **Static/low-motion generative art**
- **Dashboard displays** (data visualizations, status screens)
- **Slideshow mode** (triggered scene changes)
- **Prototyping/testing** before committing to more complex approaches

### Implementation Effort
- **ESP32 code**: 4-6 hours (new endpoint, multipart parsing, file write logic)
- **TouchDesigner script**: 1-2 hours (Python script + DAT wiring)
- **Testing**: 2-3 hours
- **Total**: ~1-2 days

---

## Option 2: HTTP POST with Direct Memory Display (RAM Buffer)

### Architecture

```
TouchDesigner                    P3A Device
┌──────────────┐                ┌─────────────────────────┐
│ TOP Network  │   HTTP POST    │  HTTP API Server        │
│ (Web Client) ├───────────────►│                         │
│              │  raw binary    │  ┌───────────────────┐  │
│  - Encode    │                │  │ POST /display/    │  │
│    image     │                │  │ raw               │  │
└──────────────┘                │  └────────┬──────────┘  │
                                │           │             │
                                │           ▼             │
                                │  ┌────────────────┐     │
                                │  │ Allocate RAM   │     │
                                │  │ buffer         │     │
                                │  │ (heap_caps_    │     │
                                │  │  malloc)       │     │
                                │  └────────┬───────┘     │
                                │           │             │
                                │           ▼             │
                                │  ┌────────────────┐     │
                                │  │ Receive chunks │     │
                                │  │ directly to    │     │
                                │  │ buffer         │     │
                                │  └────────┬───────┘     │
                                │           │             │
                                │           ▼             │
                                │  ┌────────────────┐     │
                                │  │ animation_     │     │
                                │  │ player_load_   │     │
                                │  │ from_memory()  │     │
                                │  │ [NEW API]      │     │
                                │  └────────┬───────┘     │
                                │           │             │
                                │           ▼             │
                                │  ┌────────────────┐     │
                                │  │ Decode &       │     │
                                │  │ Display        │     │
                                │  └────────────────┘     │
                                └─────────────────────────┘
```

### Implementation Details

#### ESP32 Side Changes

**1. New HTTP Endpoint** (`components/http_api/http_api.c`)
```c
POST /display/raw
Content-Type: application/octet-stream or image/png or image/jpeg
X-Image-Format: png | jpeg | webp
Content-Length: <file_size>

Request Body: Raw binary image data

Response:
{
  "ok": true,
  "data": {
    "bytes_received": 123456,
    "format": "png",
    "display_updated": true,
    "decode_time_ms": 45
  }
}
```

**2. New Animation Player API** (`main/animation_player.c`)
```c
/**
 * @brief Load and display animation from memory buffer (bypass SD card)
 *
 * @param data Pointer to image file data in RAM
 * @param size Size of image data in bytes
 * @param type Asset type (ASSET_TYPE_PNG, ASSET_TYPE_JPEG, etc.)
 * @return ESP_OK on success
 */
esp_err_t animation_player_load_from_memory(const uint8_t *data, size_t size, asset_type_t type);
```

**3. Memory Management Strategy**
```c
// Global state for live upload buffer
static struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
    SemaphoreHandle_t lock;
} s_live_buffer = {0};

// On endpoint handler:
// 1. Allocate buffer once (reuse across uploads)
// 2. Receive chunks directly to buffer
// 3. Pass pointer to animation player (decoder copies as needed)
// 4. Animation player does NOT free buffer (endpoint owns it)
```

**4. Modified Animation Player Logic**
- Add new code path: `if (source == MEMORY)` vs `if (source == SD_CARD)`
- Skip file I/O, use provided buffer directly
- Decoder already accepts memory pointers (`const uint8_t *data`) - no changes needed there
- After decode complete, memory buffer can be freed or reused for next upload

#### TouchDesigner Side

**Same as Option 1**, but simpler request:
```python
def send_raw_image_to_p3a(top_operator, ip_address='p3a.local', format='png'):
    img_array = top_operator.numpyArray(delayed=False)
    img_array = np.flipud(img_array)
    img = Image.fromarray((img_array * 255).astype(np.uint8))

    if img.size != (720, 720):
        img = img.resize((720, 720), Image.LANCZOS)

    buffer = io.BytesIO()
    img.save(buffer, format=format.upper())
    data = buffer.getvalue()

    url = f'http://{ip_address}/display/raw'
    headers = {
        'Content-Type': f'image/{format}',
        'X-Image-Format': format
    }

    response = requests.post(url, data=data, headers=headers, timeout=3)
    return response.ok
```

### Performance Characteristics

**Latency Breakdown** (720×720 PNG, ~500KB):
- TouchDesigner encode: ~10-30ms
- Network transfer: ~20-50ms
- **SD card write: 0ms** ⚡ (eliminated)
- Decode + display: ~30-100ms
- **Total: ~60-180ms per frame** → **5-15 fps achievable**

**With JPEG optimization** (~100KB file):
- Total: ~40-100ms → **10-25 fps achievable**

### Pros
✅ **2-3× faster than Option 1** (no SD bottleneck)
✅ **No SD card wear** (pure RAM operation)
✅ **Better frame rates** (10-25 fps with optimization)
✅ **Still uses existing decoder infrastructure**
✅ **Moderate implementation complexity**

### Cons
❌ **Higher RAM usage** (~500KB-2MB per buffer)
❌ **Images don't persist** (lost on reboot unless separately saved)
❌ **Requires animation player refactoring** (new API surface)
❌ **Memory fragmentation risk** (large allocations in heap)
❌ **Still not true streaming** (full image upload per frame)

### Best Use Cases
- **Semi-real-time visuals** (10-20 fps generative art)
- **Interactive installations** (user input with <100ms response)
- **Live TouchDesigner scenes** with moderate motion
- **Event-driven displays** (triggered animations, reactive visuals)

### Implementation Effort
- **ESP32 code**: 8-12 hours (new endpoint, animation player API changes, memory management)
- **TouchDesigner script**: 1-2 hours
- **Testing**: 4-5 hours (memory leak testing, heap monitoring)
- **Total**: ~2-3 days

---

## Option 3: WebSocket Streaming (Real-Time)

### Architecture

```
TouchDesigner                    P3A Device
┌──────────────┐                ┌─────────────────────────┐
│              │  WebSocket     │                         │
│ WebSocket    │  Connection    │  WebSocket Server       │
│ DAT          │◄──────────────►│  (Port 81)              │
│              │                │                         │
│  ┌────────┐  │                │  ┌──────────────────┐   │
│  │ TOP    │  │  Binary Frame  │  │ Frame Handler    │   │
│  │ Encode │──┼───────────────►│  │ Task             │   │
│  │ (JPEG) │  │  Opcode: 0x02  │  └────────┬─────────┘   │
│  └────────┘  │                │           │             │
│              │                │           ▼             │
│  - Resize    │                │  ┌────────────────────┐ │
│  - Format    │                │  │ Ring Buffer        │ │
│  - Stream    │                │  │ (3-frame depth)    │ │
└──────────────┘                │  └────────┬───────────┘ │
                                │           │             │
                                │           ▼             │
                                │  ┌────────────────────┐ │
                                │  │ Decoder Task       │ │
                                │  │ (JPEG-only)        │ │
                                │  └────────┬───────────┘ │
                                │           │             │
                                │           ▼             │
                                │  ┌────────────────────┐ │
                                │  │ Direct LCD Blit    │ │
                                │  │ (bypass animation  │ │
                                │  │  player)           │ │
                                │  └────────────────────┘ │
                                └─────────────────────────┘
```

### Implementation Details

#### ESP32 Side Changes

**1. WebSocket Server Integration**
- Use ESP-IDF HTTP server WebSocket support (`esp_http_server.h`)
- Single endpoint: `ws://p3a.local:81/stream`
- Binary frame protocol (Opcode 0x02)

**2. Frame Protocol**
```
WebSocket Binary Frame Format:
┌────────────────────────────────────────────┐
│ Header (8 bytes)                           │
├────────────────────────────────────────────┤
│ Magic: 0x5044 (2 bytes, "TD")             │
│ Format: 0x01=JPEG (1 byte)                │
│ Flags: 0x00 (1 byte, reserved)            │
│ Frame Size: (4 bytes, little-endian)      │
├────────────────────────────────────────────┤
│ Image Data (JPEG compressed)              │
│ ...                                        │
└────────────────────────────────────────────┘
```

**3. Streaming Pipeline Architecture**
```c
// Three-task pipeline:
// 1. WebSocket Receiver Task (priority 6)
//    - Receives frames, validates header
//    - Pushes to ring buffer (non-blocking)
//    - Drops frames if buffer full (lossy streaming)

// 2. Decoder Task (priority 5)
//    - Pops frames from ring buffer
//    - Decodes JPEG directly to RGB565 LCD buffer
//    - Uses hardware JPEG decoder if available (ESP32-P4 has one)

// 3. Display Task (priority 4)
//    - Blits decoded buffer to LCD via DMA
//    - Double-buffered (swap on vsync)

// Ring buffer depth: 3 frames
// - Allows 2-frame decode latency while receiving frame 3
// - Total buffering: ~300KB (3 × 100KB JPEG frames)
```

**4. Fast JPEG Decode Path**
- ESP32-P4 has **hardware JPEG decoder** (TJpgDec library wrapper)
- Direct decode to RGB565 (skip RGBA intermediate)
- Possible optimization: Decode to downscaled resolution if needed

**5. New Configuration Options** (Kconfig)
```
CONFIG_P3A_WEBSOCKET_ENABLE=y
CONFIG_P3A_STREAM_RING_BUFFER_DEPTH=3
CONFIG_P3A_STREAM_MAX_FRAME_SIZE=200KB
CONFIG_P3A_STREAM_JPEG_QUALITY=75
```

#### TouchDesigner Side

**WebSocket DAT Setup**
```python
# In WebSocket DAT's "DAT Execute" callback:
def onConnect(dat):
    print("Connected to P3A display")
    dat.par.active = True

def onDisconnect(dat):
    print("Disconnected from P3A")
    # Auto-reconnect logic here

# In separate Script DAT (called by Timer CHOP at 30 fps):
import struct

def send_frame_to_p3a(top_operator, websocket_dat):
    """Stream frame to P3A via WebSocket"""
    # Get image from TOP
    img_array = top_operator.numpyArray(delayed=False)
    img_array = np.flipud(img_array)
    img = Image.fromarray((img_array * 255).astype(np.uint8))

    # Resize to 720x720
    if img.size != (720, 720):
        img = img.resize((720, 720), Image.LANCZOS)

    # Encode to JPEG (quality=75 for balance)
    buffer = io.BytesIO()
    img.save(buffer, format='JPEG', quality=75, optimize=True)
    jpeg_data = buffer.getvalue()

    # Build protocol header
    header = struct.pack('<HHBBI',
        0x5044,           # Magic "TD"
        0x01,             # Format: JPEG
        0x00,             # Flags
        len(jpeg_data)    # Frame size
    )

    # Send as binary WebSocket frame
    frame = header + jpeg_data
    websocket_dat.sendBytes(frame)

# Usage: Call from Timer CHOP execute
# send_frame_to_p3a(op('render_out'), op('websocket1'))
```

**TouchDesigner Network Setup**
1. Add **WebSocket DAT**
   - Protocol: `ws://p3a.local:81/stream`
   - Mode: Client
   - Binary messages enabled

2. Add **Timer CHOP**
   - Rate: 30 Hz (or desired frame rate)
   - Execute DAT callback calls `send_frame_to_p3a()`

3. Add **TOP Network**
   - Wire rendering output to encoder function

### Performance Characteristics

**Latency Breakdown** (720×720 JPEG quality=75, ~80KB):
- TouchDesigner encode: ~8-15ms (JPEG is fast)
- Network transfer: ~10-20ms (WebSocket, persistent connection)
- Ring buffer queue: ~0-2ms (if not full)
- JPEG decode (hardware): ~5-15ms ⚡
- LCD blit: ~10-16ms (DMA transfer at 60 FPS vsync)
- **Total: ~33-68ms per frame** → **15-30 fps achievable**

**With Optimizations**:
- JPEG quality=60: ~50KB files → ~25-45ms total → **20-40 fps**
- 512×512 source (upscaled): ~30KB files → ~20-35ms → **25-50 fps**

**Theoretical Maximum** (ESP32-P4 hardware limits):
- LCD refresh: 60 Hz (16.67ms per frame)
- Network bandwidth (WiFi 6): ~50 Mbps practical
- At 50KB per frame: 800 fps theoretical (network-limited)
- **Real-world max: ~40-50 fps** (limited by decode + blit pipeline)

### Pros
✅ **True real-time streaming** (20-40 fps)
✅ **Lowest latency** (30-70ms end-to-end)
✅ **Persistent connection** (no HTTP overhead per frame)
✅ **Bi-directional communication possible** (TouchDesigner can receive status)
✅ **Efficient bandwidth** (small JPEG frames)
✅ **Hardware JPEG decode** (ESP32-P4 has dedicated peripheral)
✅ **Graceful frame dropping** (lossy streaming maintains fluidity)

### Cons
❌ **Most complex implementation** (WebSocket server, ring buffer, multi-task pipeline)
❌ **Requires animation player bypass** (new display path)
❌ **JPEG-only** (lossy compression, no transparency)
❌ **Higher CPU usage** (continuous streaming)
❌ **Connection management** (reconnect logic, error handling)
❌ **No persistence** (pure streaming, nothing saved)
❌ **More TouchDesigner scripting** (WebSocket DAT setup, binary protocol)

### Best Use Cases
- **Live generative art** (real-time 30 fps visuals)
- **Interactive installations** (immediate visual feedback)
- **VJ/performance tools** (live mixing, reactive visuals)
- **Game displays** (real-time game state rendering)
- **Video playback** (TouchDesigner as media server)
- **Data visualization dashboards** (high-refresh charts/graphs)

### Implementation Effort
- **ESP32 code**: 16-24 hours (WebSocket server, ring buffer, decoder task, LCD bypass)
- **TouchDesigner script**: 4-6 hours (WebSocket DAT, protocol implementation, error handling)
- **Testing**: 6-8 hours (stress testing, connection stability, frame drops)
- **Documentation**: 2-3 hours
- **Total**: ~1.5-2 weeks

---

## Comparison Matrix

| Feature | Option 1: HTTP POST (SD) | Option 2: HTTP POST (RAM) | Option 3: WebSocket Stream |
|---------|--------------------------|---------------------------|----------------------------|
| **Max Frame Rate** | 2-7 fps | 10-25 fps | 20-50 fps |
| **Latency** | 130-430ms | 60-180ms | 30-70ms |
| **Image Persistence** | ✅ Yes (SD card) | ❌ No | ❌ No |
| **Supported Formats** | PNG, JPEG, WebP, GIF | PNG, JPEG, WebP, GIF | JPEG only |
| **Implementation Time** | 1-2 days | 2-3 days | 1.5-2 weeks |
| **Code Complexity** | Low | Medium | High |
| **RAM Usage** | Low (~100KB) | Medium (~500KB-2MB) | Medium (~300KB) |
| **SD Card Wear** | High | None | None |
| **Network Protocol** | HTTP (stateless) | HTTP (stateless) | WebSocket (stateful) |
| **TouchDesigner Setup** | Simple (Web Client) | Simple (Web Client) | Complex (WebSocket DAT) |
| **Error Recovery** | Automatic (HTTP retry) | Automatic (HTTP retry) | Manual (reconnect logic) |
| **Bi-directional Comms** | ❌ (request/response only) | ❌ (request/response only) | ✅ Yes |
| **Best Use Case** | Periodic updates | Semi-real-time | Live streaming |

---

## Recommended Implementation Path

### Phase 1: Start with Option 1 (1-2 days)
**Why**: Quick proof-of-concept, minimal risk, validates workflow
- Implement HTTP POST endpoint with SD card persistence
- Test TouchDesigner integration
- Verify image quality and color accuracy
- Measure real-world performance on target network

**Success Criteria**:
- Successfully upload and display 720×720 image from TouchDesigner
- End-to-end latency < 500ms
- No crashes after 100 consecutive uploads

### Phase 2: Upgrade to Option 2 if needed (2-3 days)
**Triggers**:
- Option 1 proves concept but frame rate insufficient
- User wants 10-20 fps for smoother animations
- SD card writes causing reliability issues

**Incremental Changes**:
- Add new `/display/raw` endpoint (keep `/upload/image` for persistence)
- Implement `animation_player_load_from_memory()` API
- Both endpoints coexist (user chooses based on use case)

### Phase 3: Add Option 3 for Pro Features (1-2 weeks)
**Triggers**:
- Real-time performance required (>20 fps)
- Interactive installation with low latency demands
- Need bi-directional communication (status feedback to TouchDesigner)

**Architecture**:
- WebSocket server runs in parallel with HTTP server
- User can choose streaming mode vs upload mode
- Add config option: `stream_mode` = "off" | "websocket"

---

## Technical Risks & Mitigations

### Risk 1: Network Reliability (All Options)
**Risk**: WiFi packet loss, latency spikes, disconnections
**Mitigation**:
- Use 5 GHz WiFi band (less congestion)
- ESP32-C6 supports WiFi 6 (better reliability)
- Add network statistics endpoint (`GET /network/stats`)
- TouchDesigner script monitors upload failures, logs errors

### Risk 2: Memory Fragmentation (Options 2 & 3)
**Risk**: Large allocations cause heap fragmentation, eventual OOM
**Mitigation**:
- Pre-allocate buffers at startup (avoid repeated malloc/free)
- Use `heap_caps_malloc(MALLOC_CAP_SPIRAM)` if PSRAM available
- Monitor free heap via `/status` endpoint
- Add low-memory warning threshold (ESP_LOG_WARN at <100KB free)

### Risk 3: SD Card Corruption (Option 1)
**Risk**: Power loss during write, file system corruption
**Mitigation**:
- Atomic writes (write to temp file, rename on success)
- Add `fsync()` after writes
- Config option: `upload_durability` = "fast" | "safe"
- Periodic SD card health check (read test file)

### Risk 4: Image Quality/Color Accuracy
**Risk**: Color space mismatches, gamma issues, compression artifacts
**Mitigation**:
- Document color pipeline: TouchDesigner (sRGB) → ESP32 (RGB565/888)
- Add gamma correction option (Kconfig: `CONFIG_P3A_GAMMA_CORRECT=y`)
- Test pattern image for calibration
- Support for raw RGB upload (no compression) for quality testing

### Risk 5: Decoder Crashes (All Options)
**Risk**: Malformed image data crashes decoder libraries
**Mitigation**:
- Validate magic bytes before passing to decoder
- Set decoder memory limits (prevent OOM)
- Catch decoder errors, return HTTP 400 (Bad Request)
- Add watchdog timer to decoder tasks (auto-reboot on hang)

---

## Testing Plan

### Unit Tests
1. **HTTP Endpoint Tests**
   - Valid image upload (PNG, JPEG, WebP)
   - Malformed data (truncated, wrong magic bytes)
   - Oversized files (>5MB)
   - Concurrent uploads (queue behavior)

2. **Memory Tests**
   - Upload 1000 images, monitor heap (check for leaks)
   - Stress test: Max-size images repeatedly
   - Low-memory scenario: Upload when heap < 200KB

3. **Network Tests**
   - Upload over slow connection (simulate via bandwidth throttling)
   - WiFi disconnection during upload
   - Multiple clients uploading simultaneously

### Integration Tests
1. **TouchDesigner → ESP32 End-to-End**
   - Render simple pattern (checkerboard, gradients)
   - Verify color accuracy (use colorimeter if available)
   - Measure latency (timestamp in image, compare display photo)

2. **Format Compatibility**
   - Test all supported formats (PNG/JPEG/WebP/GIF)
   - Test edge cases: 1×1 pixel, 4096×4096 (should reject), grayscale

3. **Endurance Testing**
   - Continuous upload for 24 hours
   - Monitor device temperature (thermal throttling?)
   - Check for memory leaks, file descriptor leaks

### Performance Benchmarks
- Measure each latency stage (encode, transfer, decode, display)
- Test at various image sizes: 256×256, 512×512, 720×720, 1024×1024
- Test JPEG quality levels: 50, 70, 85, 95
- Measure frame rate under continuous streaming

---

## Configuration Options (Kconfig)

```kconfig
menu "TouchDesigner Integration"

config P3A_UPLOAD_ENABLE
    bool "Enable image upload endpoints"
    default y
    help
        Enable HTTP endpoints for uploading images from TouchDesigner

config P3A_UPLOAD_MAX_FILE_SIZE
    int "Maximum upload file size (bytes)"
    default 5242880
    range 102400 10485760
    depends on P3A_UPLOAD_ENABLE
    help
        Maximum size for uploaded image files (default: 5MB)

config P3A_UPLOAD_SAVE_PATH
    string "Upload save path"
    default "/sdcard/animations/td_live"
    depends on P3A_UPLOAD_ENABLE
    help
        Base path for saving uploaded images (extension added automatically)

config P3A_UPLOAD_OVERWRITE
    bool "Overwrite existing upload file"
    default y
    depends on P3A_UPLOAD_ENABLE
    help
        If enabled, uploads overwrite the previous file. If disabled, adds timestamp.

config P3A_STREAM_ENABLE
    bool "Enable WebSocket streaming"
    default n
    help
        Enable WebSocket server for real-time image streaming from TouchDesigner

config P3A_STREAM_PORT
    int "WebSocket server port"
    default 81
    depends on P3A_STREAM_ENABLE
    help
        Port for WebSocket streaming server

config P3A_STREAM_RING_BUFFER_DEPTH
    int "Stream ring buffer depth (frames)"
    default 3
    range 1 10
    depends on P3A_STREAM_ENABLE
    help
        Number of frames to buffer for streaming pipeline

config P3A_STREAM_JPEG_ONLY
    bool "Restrict streaming to JPEG only"
    default y
    depends on P3A_STREAM_ENABLE
    help
        For performance, only allow JPEG streaming (not PNG/WebP)

endmenu
```

---

## TouchDesigner Quick Start Guide

This section provides a complete walkthrough for TouchDesigner users to get started with P3A display integration.

### Prerequisites

1. **TouchDesigner Installation**
   - TouchDesigner 2023 or later recommended
   - Free version (Non-Commercial) works fine

2. **P3A Display**
   - Device powered on and connected to same WiFi network
   - Note device IP address or use hostname `p3a.local`
   - Verify HTTP server is running (browse to `http://p3a.local/`)

3. **Python Dependencies** (for Method A - Python Script approach)
   - Install via tdPyEnvManager: `requests`, `pillow`, `numpy`

### Quick Start: Option 1 Implementation (HTTP POST)

**Step 1: Create Project Structure**

```
your_project.toe
├── scripts/
│   └── p3a_upload_http.py  (save the Python script from Method A above)
```

**Step 2: Setup TouchDesigner Network**

Create the following operators in your TouchDesigner network:

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│ Render TOP  │────►│ Timer CHOP   │────►│ Execute DAT │
│ (your work) │     │ (upload rate)│     │ (trigger)   │
└─────────────┘     └──────────────┘     └─────────────┘
```

**Step 3: Configure Timer CHOP**

- **Name**: `upload_timer`
- **Rate**: `2` (uploads 2 times per second - adjust as needed)
- **Length**: `1000000`
- **Mode**: `Timer`
- **Clock Chop**: (leave empty)

**Step 4: Configure Execute DAT**

- **Name**: `upload_execute`
- **Trigger Parameters**:
  - **CHOPs**: `upload_timer`
  - **Value Change**: ON
  - **Off to On**: ON

**Execute DAT Code**:
```python
def onValueChange(channel, sampleIndex, val, prev):
    """Triggered when timer CHOP pulses"""
    if val > 0:  # On pulse
        exec(open(r'scripts/p3a_upload_http.py').read())
```

**Step 5: Edit Upload Script**

Modify `scripts/p3a_upload_http.py` to point to your render TOP:

```python
# At the bottom of p3a_upload_http.py, change:
upload_to_p3a(
    top_path='your_render_top_name',  # ← Change this to your TOP's name
    ip_address='p3a.local',            # Or use IP like '192.168.1.100'
    format='JPEG',                     # PNG for quality, JPEG for speed
    quality=75                         # Lower = smaller files = faster
)
```

**Step 6: Test Upload**

In TouchDesigner textport, run:
```python
exec(open(r'scripts/p3a_upload_http.py').read())
```

You should see:
```
Uploading 85432 bytes to http://p3a.local/upload/image...
✓ Upload successful: {'ok': True, 'saved_path': '/sdcard/animations/td_live.jpg', ...}
```

**Step 7: Start Continuous Upload**

- Enable the Timer CHOP (set `Active` parameter to ON)
- Your renders will now upload to P3A at the specified rate
- Monitor textport for upload status messages

### Troubleshooting Common Issues

#### Issue: "TOP 'render_out' not found"
**Solution**: Check the exact name of your TOP operator in TouchDesigner. Names are case-sensitive.
```python
# Find your TOP name:
print([op for op in ops() if op.type == 'TOP'])
```

#### Issue: "No module named 'requests'"
**Solution**: Install Python packages via tdPyEnvManager:
1. Open Palette (Alt+L)
2. Search for "tdPyEnvManager"
3. Drag to network, open component
4. Install: `requests`, `pillow`, `numpy`

#### Issue: "Connection refused" or "Timeout"
**Solution**: Verify P3A network connectivity:
- Ping device: `ping p3a.local` or `ping <ip_address>`
- Check device is on same WiFi network
- Try using IP address instead of hostname
- Verify HTTP server running on P3A (browse to web UI)

#### Issue: Upload too slow / frame rate low
**Solution**: Optimize image format and size:
- Use JPEG instead of PNG (10× smaller files)
- Lower JPEG quality: `quality=60` or `quality=50`
- Reduce resolution before upload: Resize TOP to 512×512 or 360×360
- Increase Timer CHOP interval (reduce upload rate)

#### Issue: "Upload error: SSLError"
**Solution**: P3A uses HTTP (not HTTPS). Verify URL:
```python
# Wrong:
url = 'https://p3a.local/upload/image'

# Correct:
url = 'http://p3a.local/upload/image'
```

#### Issue: Textport shows "IndentationError"
**Solution**: DO NOT run multi-line code directly in textport. Always save to file and use:
```python
exec(open(r'scripts/your_script.py').read())
```

### Performance Tuning Tips

#### For Maximum Quality (Static/Slow Updates)
```python
upload_to_p3a(
    top_path='your_top',
    format='PNG',              # Lossless compression
    # No quality parameter needed for PNG
)
# Timer CHOP rate: 0.5 (once every 2 seconds)
```

#### For Maximum Speed (Real-Time Updates)
```python
upload_to_p3a(
    top_path='your_top',
    format='JPEG',
    quality=60                 # Lower quality = smaller files
)
# Timer CHOP rate: 5 (5 times per second)
# Resize TOP to 512×512 before upload for even faster performance
```

#### For Balanced Performance
```python
upload_to_p3a(
    top_path='your_top',
    format='JPEG',
    quality=85                 # Good quality, reasonable size
)
# Timer CHOP rate: 2 (2 times per second)
```

### Advanced: Non-Blocking Uploads with Thread Manager

For continuous rendering without blocking TouchDesigner's UI thread, use TouchDesigner's Thread Manager:

**Setup**:
1. Research Thread Manager documentation: https://docs.derivative.ca/Thread_Manager
2. Modify upload script to run in background thread
3. Handle callbacks for upload completion/failure
4. Maintain upload queue to avoid overwhelming P3A device

**Benefits**:
- TouchDesigner UI remains responsive during upload
- Can achieve higher upload rates without stuttering
- Better error handling and retry logic

### Example .toe File Structure

```
project1/
├── render/               # Your generative art network
│   ├── noise1 (TOP)
│   ├── feedback1 (TOP)
│   └── composite1 (TOP)  # Final render output
│
├── p3a_integration/      # P3A upload system
│   ├── upload_timer (CHOP)
│   ├── upload_execute (DAT)
│   └── upload_status (TEXT DAT)
│
└── scripts/
    └── p3a_upload_http.py
```

### Next Steps

Once Option 1 is working:
- **Option 2**: Implement if you need faster updates (10-20 fps)
- **Option 3**: Implement WebSocket streaming for true real-time (30+ fps)
- **Bi-directional**: Have P3A send touch events back to TouchDesigner
- **Multi-Display**: Send to multiple P3A devices simultaneously

---

## Documentation Deliverables

1. **User Guide**: `docs/TOUCHDESIGNER_GUIDE.md`
   - TouchDesigner setup instructions
   - Example .toe file (network setup)
   - Python script templates
   - Troubleshooting common issues

2. **API Reference**: `docs/API_UPLOAD.md`
   - HTTP endpoint documentation
   - WebSocket protocol specification
   - Error codes reference
   - Example curl commands

3. **Performance Guide**: `docs/PERFORMANCE_TUNING.md`
   - Image size/format recommendations
   - Network optimization tips
   - Latency measurement tools
   - Benchmark results

---

## Future Enhancements (Post-MVP)

### Additional Features
1. **Multi-Region Update** (partial screen updates)
   - Upload only changed regions, not full frame
   - Protocol: Include bounding box in header
   - 5-10× bandwidth reduction for localized changes

2. **Compressed Frame Streaming** (custom format)
   - Delta frames (only send differences)
   - Custom codec optimized for pixel art (palette-based)
   - Potential for 60 fps at low bandwidth

3. **Audio Sync** (for music visualizers)
   - WebSocket carries both video frames + audio waveform data
   - ESP32 displays in sync with TouchDesigner audio output

4. **Bi-directional Control**
   - ESP32 sends touch events back to TouchDesigner
   - Interactive installations (touch display, affects generative algorithms)

5. **Multi-Display Support**
   - TouchDesigner streams to multiple P3A devices
   - Synchronized playback (time-code based)
   - Large-scale installations (video wall of P3A panels)

---

## Conclusion

All three options are technically feasible and offer different trade-offs:

- **Option 1** is the fastest to implement and most reliable, suitable for 90% of use cases
- **Option 2** provides better performance without excessive complexity
- **Option 3** unlocks true real-time capabilities but requires significant engineering effort

**Recommended approach**: Start with Option 1, measure performance, upgrade incrementally if needed.

---

## References

### ESP-IDF Documentation
- [ESP HTTP Server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- [ESP WebSocket](https://github.com/espressif/esp-idf/tree/master/examples/protocols/http_server/ws_echo_server)
- [JPEG Decoder](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/jpeg.html)

### TouchDesigner Documentation
- [Web Client DAT](https://docs.derivative.ca/Web_Client_DAT)
- [WebSocket DAT](https://docs.derivative.ca/WebSocket_DAT)
- [Python TOP Export](https://docs.derivative.ca/TOP_Class)

### Related Work
- [ESP32 HTTP File Upload Example](https://github.com/espressif/esp-idf/tree/master/examples/protocols/http_server/file_serving)
- [TouchDesigner Network Protocols](https://docs.derivative.ca/Category:DATs)
