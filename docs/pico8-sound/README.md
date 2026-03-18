# PICO-8 Sound Streaming Plan

Play PICO-8 game audio on the browser, on the p3a speaker, or both -- independently togglable.

## Current State

### What exists

- **Video streaming works**: Browser runs Fake-08 WASM emulator, encodes 128x128 4bpp frames, streams them over WebSocket (`/pico_stream`) to ESP32, which upscales and displays at 60fps.
- **Speaker hardware is wired**: ES8311 DAC -> NS4150B Class-D amp -> 8-ohm 2W speaker. All on-board, pre-connected.
- **BSP audio drivers exist**: `bsp_audio_codec_speaker_init()` returns a ready-to-use `esp_codec_dev_handle_t`. Accepts raw PCM (16-bit, mono, 8-96 kHz).
- **Audio is not initialized**: No call to `bsp_audio_init()` or `bsp_audio_codec_speaker_init()` anywhere in p3a firmware.

### What's missing

- **Fake-08 WASM has no audio output**: The current build exposes no audio functions (`_f08_get_audio*` etc.). No Web Audio API, no SDL audio, no Emscripten audio hooks. The emulator was compiled as video-only.
- **No audio pipeline on ESP32**: No I2S init, no codec init, no audio playback task.
- **No UI toggles**: No audio controls on the PICO-8 web page.

## Approach: Browser-Side Audio Capture

Since Fake-08 already runs in the browser, the most practical approach is:

1. **Rebuild Fake-08 WASM with audio synthesis enabled** -- expose a function that fills a PCM buffer with the emulator's audio output.
2. **Two independent audio sinks controlled by two toggles**:
   - **"Play sound here"** (browser) -- feeds samples into the Web Audio API so the user hears audio from their device.
   - **"Play sound on p3a"** (device) -- streams the same samples over the existing WebSocket to the ESP32 speaker.
3. Both toggles can be ON simultaneously (hear audio on both), or only one, or neither.
4. The audio source (Fake-08 synth) always runs when either toggle is ON; it is paused when both are OFF to save CPU.

### Why not re-synthesize on ESP32?

Duplicating the PICO-8 audio engine (4-channel synth with 8 waveforms, SFX editor patterns, music tracker) on ESP32 would be a massive effort. Streaming raw PCM from the browser is simple, proven, and reuses the existing emulator.

### Why not a separate WebSocket?

Using the same `/pico_stream` WebSocket avoids consuming another socket from the tight 12-socket budget, keeps audio and video in sync, and simplifies connection lifecycle management.

## Audio Bandwidth Analysis

| Parameter | Value |
|-----------|-------|
| PICO-8 native sample rate | 22050 Hz |
| Bit depth | 16-bit |
| Channels | Mono |
| Raw throughput | 22050 x 2 = **44,100 bytes/sec** |
| Per video frame (30fps) | ~1,470 bytes |
| With IMA-ADPCM (4:1) | ~11,025 bytes/sec (~368 bytes/frame) |
| Video frame size | ~8,246 bytes |
| Audio overhead vs video | ~4.5% (ADPCM) to ~18% (raw PCM) |

**Verdict**: Even raw PCM is very manageable. ADPCM compression is an optimization that can be added later if needed.

---

## Implementation Plan

### Phase 1: Rebuild Fake-08 WASM with Audio

**Goal**: Make the emulator produce audio samples accessible from JavaScript.

#### 1.1 Fork/patch Fake-08 audio integration

The upstream [Fake-08](https://github.com/jtothebell/fake-08) already has an audio synthesis engine (it runs on real hardware like 3DS, Vita, etc.). The current p3a WASM build simply disabled or omitted the audio backend.

**Tasks**:
- Clone the Fake-08 source used to build the current WASM.
- Identify the audio synthesis module (likely `Audio.cpp` or similar -- Fake-08 uses a platform-agnostic synth that fills a PCM buffer each frame).
- Create an Emscripten-compatible audio backend, either:
  - **Option A (simpler)**: Export a C function `f08_get_audio_buffer(int16_t *buf, int max_samples)` that fills a buffer with the next N audio samples. The JS side calls it each frame and handles Web Audio output + streaming.
  - **Option B**: Enable Emscripten's SDL2 audio, which auto-creates a `ScriptProcessorNode`. Then intercept the audio data in JS.
- Rebuild with `emcc` including audio support.

**Option A is recommended** because it gives full control over the audio pipeline in JavaScript, avoids SDL audio threading issues in the browser, and lets us easily toggle streaming on/off.

#### 1.2 New Fake-08 WASM exports

Add these functions to the WASM module:

```c
// Fill buffer with audio samples generated since last call.
// Returns number of samples written (mono, 16-bit signed).
int _f08_fill_audio_buffer(int16_t *buffer, int max_samples);

// Get the native audio sample rate (expected: 22050).
int _f08_get_audio_sample_rate(void);
```

#### 1.3 Rebuild and replace

- Compile with Emscripten (`emcc`), producing updated `fake08.js` + `fake08.wasm`.
- Replace the files in `webui/static/`.
- Verify video-only mode still works identically.

**Files changed**: `webui/static/fake08.js`, `webui/static/fake08.wasm`

---

### Phase 2: Browser-Side Audio Pipeline

**Goal**: Generate audio samples from the emulator and route them to two independent sinks.

#### 2.1 Audio source (always shared)

After each `_f08_step_frame()`, call `_f08_fill_audio_buffer()` to get ~368 samples (22050 Hz / 60fps). This call is made whenever **at least one** of the two toggles is ON. When both toggles are OFF the call is skipped entirely.

```
_f08_fill_audio_buffer()
        |
        +---> [if browserAudio ON]  --> Web Audio API --> browser speakers
        |
        +---> [if deviceAudio ON]   --> sendAudioPacket() --> WebSocket --> ESP32 speaker
```

#### 2.2 Sink 1: Browser audio ("Play sound here")

Feed the PCM samples into a Web Audio API pipeline:

```
PCM samples -> AudioBufferSourceNode (per chunk) -> GainNode -> AudioContext.destination
```

- Create the `AudioContext` lazily on the first toggle-ON (browsers require a user gesture to create an AudioContext -- the toggle click satisfies this).
- When toggled OFF: disconnect the GainNode output (or set gain to 0). Don't close the AudioContext -- keep it alive so toggling back ON is instant.
- When both toggles go OFF: suspend the AudioContext to release audio thread resources.

#### 2.3 Sink 2: Device audio ("Play sound on p3a")

When this toggle is ON, the same PCM samples are also sent over the existing WebSocket, interleaved with video frames.

**New packet type** -- `p8A` (Audio):

```
Offset  Size  Field          Description
------  ----  -----          -----------
0       3     Magic          0x70 0x38 0x41 ("p8A")
3       2     Payload Len    Little-endian uint16 (audio data only)
5       1     Flags          0x00 (reserved for future: compression type, etc.)
6       N     Audio Data     Raw PCM: mono, 16-bit signed LE, 22050 Hz
```

Typical packet size: 6 header + ~736 bytes PCM = ~742 bytes per video frame. Well within the existing 512-byte stack buffer for small payloads or the heap allocation path.

#### 2.4 Adaptive rate interaction

Audio packets are much smaller than video frames (~742 vs ~8,246 bytes), so they add minimal congestion pressure. The adaptive rate control in `computeAdaptiveRate()` doesn't need changes -- it already measures `ws.bufferedAmount` which will naturally account for audio data.

If the connection is severely congested (high skip interval), audio frames should still be sent at the video frame rate to stay in sync. Audio without video is acceptable; video without audio is also acceptable. They don't need to be atomically linked.

**Files changed**: `webui/static/pico8.js`

---

### Phase 3: ESP32 Audio Subsystem

**Goal**: Initialize the speaker hardware and play incoming audio samples.

#### 3.1 Create `pico8_audio` module

New files in `components/pico8/`:

| File | Purpose |
|------|---------|
| `pico8_audio.c` | Audio ring buffer, I2S feed task, init/deinit |
| `include/pico8_audio.h` | Public API |

**Public API**:

```c
// Initialize audio hardware (ES8311 + I2S). Called once during boot or lazily on first use.
esp_err_t pico8_audio_init(void);

// Start audio playback (called when entering PICO-8 mode with sound enabled).
esp_err_t pico8_audio_start(void);

// Submit PCM samples from WebSocket handler. Thread-safe (copies into ring buffer).
esp_err_t pico8_audio_feed(const int16_t *samples, size_t num_samples);

// Stop audio playback and drain buffer. Called when exiting PICO-8 mode.
void pico8_audio_stop(void);

// Check if audio is currently active.
bool pico8_audio_is_active(void);
```

#### 3.2 Ring buffer design

```
WebSocket handler                    Audio feed task
(runs in HTTP server task)           (dedicated FreeRTOS task)
        |                                    |
        v                                    v
  pico8_audio_feed()  -->  [Ring Buffer]  -->  esp_codec_dev_write()
  (producer)               (4-8 KB)           (consumer, blocks until I2S DMA accepts)
```

- **Ring buffer size**: 4096-8192 bytes (~93-186 ms at 22050 Hz mono 16-bit). This absorbs jitter in WebSocket delivery.
- **Implementation**: Use FreeRTOS `StreamBuffer` (lockless single-producer single-consumer) for zero-copy efficiency.
- **Underrun handling**: Feed silence (zeros) to avoid pops/clicks. Log underruns at debug level.
- **Overrun handling**: Drop oldest samples (ring buffer naturally handles this).

#### 3.3 Audio feed task

A dedicated FreeRTOS task that:
1. Waits for data in the ring buffer (blocks with timeout).
2. Reads a chunk (e.g., 512 bytes = ~11.6 ms).
3. Calls `esp_codec_dev_write()` which blocks until I2S DMA accepts the data.
4. Loops.

Task configuration:
- Stack: 3072 bytes (codec writes are simple DMA transfers)
- Priority: higher than HTTP server (tskIDLE_PRIORITY + 5) to prevent audio starvation
- Core: no affinity (let scheduler decide)
- Allocated from internal RAM (not PSRAM, for latency)

#### 3.4 Hardware initialization

On first `pico8_audio_start()` (lazy init):

```c
// 1. Initialize I2S and ES8311 codec via BSP
esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();

// 2. Configure sample format
esp_codec_dev_sample_info_t fs = {
    .sample_rate = 22050,
    .channel = 1,
    .bits_per_sample = 16,
};
esp_codec_dev_open(speaker, &fs);

// 3. Set volume (could be configurable later)
esp_codec_dev_set_out_vol(speaker, 60);
```

The NS4150B amplifier is enabled by the BSP via GPIO53 (active HIGH) during `bsp_audio_codec_speaker_init()`.

#### 3.5 Kconfig options

Add to `components/pico8/Kconfig`:

```kconfig
config P3A_PICO8_AUDIO_ENABLE
    bool "Enable PICO-8 audio streaming"
    default y
    depends on P3A_PICO8_ENABLE
    help
        Enable streaming PICO-8 game audio to the on-board speaker.
        Requires the ES8311 audio codec and NS4150B amplifier.

config P3A_PICO8_AUDIO_VOLUME
    int "Default PICO-8 audio volume (0-100)"
    default 60
    range 0 100
    depends on P3A_PICO8_AUDIO_ENABLE
    help
        Default volume for PICO-8 audio playback.

config P3A_PICO8_AUDIO_BUFFER_SIZE
    int "Audio ring buffer size (bytes)"
    default 4096
    range 2048 16384
    depends on P3A_PICO8_AUDIO_ENABLE
    help
        Size of the ring buffer for audio sample delivery.
        Larger values absorb more jitter but add latency.
```

**Files changed**: `components/pico8/pico8_audio.c`, `components/pico8/include/pico8_audio.h`, `components/pico8/CMakeLists.txt`, `components/pico8/Kconfig`

---

### Phase 4: WebSocket Audio Handler

**Goal**: Route incoming `p8A` audio packets to the audio subsystem.

#### 4.1 Extend `http_api_pico8.c`

In `h_ws_pico_stream()`, after the existing `p8F` (video frame) handling, add a branch for `p8A` (audio):

```c
// Audio packet: "p8A" (0x70 0x38 0x41)
if (frame.payload[0] == 0x70 && frame.payload[1] == 0x38 && frame.payload[2] == 0x41) {
    // Parse header
    uint16_t audio_len = (uint16_t)frame.payload[3] | ((uint16_t)frame.payload[4] << 8);
    // uint8_t flags = frame.payload[5]; // reserved

    if (frame.len >= 6 + audio_len) {
        pico8_audio_feed((const int16_t *)(frame.payload + 6), audio_len / 2);
    }
}
```

Audio packets are small (~742 bytes), so they'll use the existing 512-byte stack buffer path (for header) or small heap allocation. No changes to `WS_MAX_FRAME_SIZE` needed.

#### 4.2 Mode lifecycle integration

In `pico8_stream_enter_mode()`:
```c
#if CONFIG_P3A_PICO8_AUDIO_ENABLE
    pico8_audio_start();  // Prepare audio pipeline (actual playback starts when first samples arrive)
#endif
```

In `pico8_stream_exit_mode()`:
```c
#if CONFIG_P3A_PICO8_AUDIO_ENABLE
    pico8_audio_stop();   // Stop playback, drain buffer, disable amp
#endif
```

**Files changed**: `components/http_api/http_api_pico8.c`, `components/pico8/pico8_stream.c`

---

### Phase 5: Web UI Toggles

**Goal**: Add two independent audio toggles to the PICO-8 page.

#### 5.1 HTML toggles

Add a new "Sound" section to `webui/pico8/index.html`, inside the `.controls-panel`, between the "Load Cart" and "Status" sections:

```html
<div class="load-section" id="sound-section" style="display:none;">
    <h2>Sound</h2>
    <div class="toggle-row">
        <label for="browser-sound-toggle" class="toggle-label">Play sound here</label>
        <label class="toggle-switch">
            <input type="checkbox" id="browser-sound-toggle">
            <span class="toggle-slider"></span>
        </label>
    </div>
    <div class="toggle-row">
        <label for="device-sound-toggle" class="toggle-label">Play sound on p3a</label>
        <label class="toggle-switch">
            <input type="checkbox" id="device-sound-toggle">
            <span class="toggle-slider"></span>
        </label>
    </div>
</div>
```

The entire Sound section is hidden by default and shown only when the WASM module confirms audio support (i.e., `_f08_fill_audio_buffer` exists). This ensures the old WASM (no audio exports) works exactly as before with no visible changes.

#### 5.2 JavaScript state variables

```javascript
let browserAudio = false;   // "Play sound here" toggle state
let deviceAudio = false;    // "Play sound on p3a" toggle state
let audioCtx = null;        // Web Audio context (created lazily)
let gainNode = null;        // Gain node for browser audio output
```

#### 5.3 Toggle wiring

After WASM init, check for audio support and wire up both toggles:

```javascript
if (typeof Module._f08_fill_audio_buffer === 'function') {
    document.getElementById('sound-section').style.display = '';

    // Restore saved preferences
    browserAudio = localStorage.getItem('p3a_pico8_browser_sound') === 'true';
    deviceAudio = localStorage.getItem('p3a_pico8_device_sound') === 'true';
    document.getElementById('browser-sound-toggle').checked = browserAudio;
    document.getElementById('device-sound-toggle').checked = deviceAudio;

    document.getElementById('browser-sound-toggle').addEventListener('change', (e) => {
        browserAudio = e.target.checked;
        localStorage.setItem('p3a_pico8_browser_sound', browserAudio);
        if (browserAudio) {
            ensureAudioContext();    // Lazy-create AudioContext on user gesture
        } else if (!deviceAudio) {
            suspendAudioContext();   // Both OFF -> release audio thread
        }
    });

    document.getElementById('device-sound-toggle').addEventListener('change', (e) => {
        deviceAudio = e.target.checked;
        localStorage.setItem('p3a_pico8_device_sound', deviceAudio);
        if (!deviceAudio && !browserAudio) {
            suspendAudioContext();
        }
    });
}
```

#### 5.4 Audio source in the animate loop

In the `animate()` loop, after `_f08_step_frame()`:

```javascript
// Generate audio samples only when at least one sink is active
if ((browserAudio || deviceAudio) && typeof Module._f08_fill_audio_buffer === 'function') {
    const samplesPerFrame = Module._f08_get_audio_sample_rate() / 60;
    const audioPtr = Module._malloc(samplesPerFrame * 2);
    const written = Module._f08_fill_audio_buffer(audioPtr, samplesPerFrame);

    if (written > 0) {
        const samples = new Int16Array(Module.HEAP16.buffer, audioPtr, written);

        // Sink 1: browser speakers
        if (browserAudio) {
            playLocalAudio(samples);
        }

        // Sink 2: p3a device speaker
        if (deviceAudio && ws && ws.readyState === WebSocket.OPEN) {
            sendAudioPacket(samples);
        }
    }
    Module._free(audioPtr);
}
```

#### 5.5 Audio packet sending

```javascript
function sendAudioPacket(samples) {
    const pcmBytes = new Uint8Array(samples.buffer, samples.byteOffset, samples.byteLength);
    const packet = new Uint8Array(6 + pcmBytes.length);
    packet[0] = 0x70; // 'p'
    packet[1] = 0x38; // '8'
    packet[2] = 0x41; // 'A' (p8A)
    const payloadLen = pcmBytes.length;
    packet[3] = payloadLen & 0xFF;
    packet[4] = (payloadLen >> 8) & 0xFF;
    packet[5] = 0x00; // flags (reserved)
    packet.set(pcmBytes, 6);
    ws.send(packet.buffer);
}
```

#### 5.6 Toggle state persistence

Each toggle is stored independently in `localStorage`:

| Key | Values | Default |
|-----|--------|---------|
| `p3a_pico8_browser_sound` | `"true"` / `"false"` | `false` |
| `p3a_pico8_device_sound` | `"true"` / `"false"` | `false` |

No NVS/config_store change needed -- these are browser-side preferences. The preference follows the browser (different browsers/devices can have different settings), which is appropriate since PICO-8 is always controlled from a browser session.

**Files changed**: `webui/pico8/index.html`, `webui/static/pico8.js`

---

### Phase 6: Testing and Polish

#### 6.1 Test matrix

| Test | Expected result |
|------|----------------|
| Load PICO-8 page, both toggles OFF | Video streams normally, no audio anywhere, speaker silent |
| Turn ON "Play sound here" only | Audio plays in browser, p3a speaker silent, no WebSocket audio packets sent |
| Turn ON "Play sound on p3a" only | Audio plays on p3a speaker, browser silent |
| Both toggles ON | Audio plays on both browser and p3a simultaneously |
| Turn OFF "Play sound on p3a" while "Play sound here" stays ON | p3a speaker stops, browser audio continues uninterrupted |
| Turn OFF "Play sound here" while "Play sound on p3a" stays ON | Browser goes silent, p3a audio continues uninterrupted |
| Both toggles ON -> both OFF | All audio stops, AudioContext suspended, p3a amp idle |
| Toggle "Play sound here" ON mid-game | Audio starts in browser within one frame, no video disruption |
| Toggle "Play sound on p3a" ON mid-game | Audio starts on p3a within ~200ms, no video disruption |
| Exit PICO-8 mode (navigate away) | All audio stops, p3a amplifier disabled, I2S idle |
| Timeout (30s no frames) | Audio stops with video on both sinks |
| Play game with no SFX/music | Silence on both sinks, no noise/artifacts |
| High network congestion ("Play sound on p3a" ON) | p3a audio may have dropouts, browser audio unaffected, video adapts |
| Reopen PICO-8 page after closing | Toggle states restored from localStorage |
| Old WASM (no audio exports) | Sound section hidden entirely, pure video mode |

#### 6.2 Latency budget

| Stage | Estimated latency |
|-------|------------------|
| WASM audio buffer fill | <1 ms |
| WebSocket send + Wi-Fi TX | 5-20 ms |
| ESP32 WebSocket receive + parse | <1 ms |
| Ring buffer -> I2S DMA | 10-25 ms (depends on buffer fill level) |
| **Total audio latency** | **~20-50 ms** |
| Video latency (existing) | ~30-80 ms |

Audio will be roughly in sync with video. Exact sync is not critical for game audio -- players are accustomed to small audio-visual offsets.

#### 6.3 Memory budget

| Component | Memory |
|-----------|--------|
| Audio ring buffer | 4 KB (configurable) |
| Audio feed task stack | 3 KB |
| I2S DMA buffers (managed by driver) | ~2 KB |
| esp_codec_dev handle | ~256 bytes |
| **Total** | **~10 KB** |

Well within the 32 MB PSRAM budget. Task stack should be in internal RAM for latency.

---

## File Change Summary

| File | Change type | Description |
|------|-------------|-------------|
| `webui/static/fake08.js` | Replace | Rebuilt WASM glue with audio exports |
| `webui/static/fake08.wasm` | Replace | Rebuilt WASM binary with audio synthesis |
| `webui/static/pico8.js` | Modify | Add audio source, two-sink routing, Web Audio API, WebSocket audio packets, toggle logic |
| `webui/pico8/index.html` | Modify | Add Sound section with two toggles ("Play sound here", "Play sound on p3a") |
| `components/pico8/pico8_audio.c` | New | Audio ring buffer, I2S feed task, init/start/stop |
| `components/pico8/include/pico8_audio.h` | New | Public API for pico8_audio |
| `components/pico8/CMakeLists.txt` | Modify | Add pico8_audio.c, add BSP dependency |
| `components/pico8/Kconfig` | Modify | Add audio enable, volume, buffer size options |
| `components/pico8/pico8_stream.c` | Modify | Call pico8_audio_start/stop on mode enter/exit |
| `components/http_api/http_api_pico8.c` | Modify | Handle `p8A` audio packets |

## Dependency Chain

```
Phase 1 (Fake-08 WASM rebuild)
    |
    v
Phase 2 (Browser audio pipeline)  <-- can be tested standalone in browser
    |
    v
Phase 3 (ESP32 audio subsystem)   <-- can be tested with synthetic audio
    |
    v
Phase 4 (WebSocket audio handler) <-- connects browser to ESP32
    |
    v
Phase 5 (Web UI toggle)           <-- user-facing control
    |
    v
Phase 6 (Testing & polish)
```

Phase 1 is the critical-path blocker: everything else depends on having a WASM build that produces audio samples. Phases 3 and 2 can be developed in parallel once Phase 1 is done.

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Fake-08 audio engine is broken or incomplete in WASM | Blocks entire feature | Test Fake-08 audio in browser first (Phase 1); if broken, consider alternative emulators or audio engines |
| I2S conflicts with other peripherals | No audio output | ESP32-P4 has 3 I2S controllers; audio uses I2S_NUM_0, display uses MIPI-DSI (not I2S) |
| WebSocket congestion from audio data | Choppy video | Audio adds only 4.5-18% bandwidth; adaptive rate control already handles congestion |
| Audio latency too high | Noticeable delay between action and sound | Target <50ms; ring buffer size is configurable; can reduce I2S DMA buffer count |
| Speaker pops on start/stop | Unpleasant user experience | Ramp volume from 0 on start; ramp to 0 before stop; ES8311 has soft-mute feature |

## Open Questions

1. **Which Fake-08 version/fork was used for the current WASM build?** Needed to reproduce the build and add audio.
2. **Is there an existing Emscripten build script for Fake-08?** Or was it a custom build?
3. **Volume control in web UI?** A slider per sink could be added alongside each toggle. This plan keeps it simple with just on/off for now; volume can be added as a follow-up.
4. **Should the toggle states be stored on the device (NVS)?** Plan uses browser localStorage, which means the preference follows the browser, not the device. This seems appropriate since PICO-8 is always controlled from a browser session.
