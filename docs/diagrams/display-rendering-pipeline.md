# p3a Display & Rendering Pipeline

This document describes how the screen is drawn: the triple-buffered display renderer, the render mode switching, the central render dispatcher, and the animation player's double-buffered decode pipeline.

## 1. Display Buffer State Machine

The display renderer (`main/display_renderer.c`) uses **triple buffering** with VSYNC synchronization. Each of the 3 frame buffers cycles through four states:

```
 ┌────────────────┐
 │  BUFFER_STATE_ │     acquire_free_buffer()
 │  FREE          │ ─────────────────────────────────────────►┌────────────────┐
 │                │                                           │  BUFFER_STATE_ │
 │ (safe to write)│                                           │  RENDERING     │
 └────────────────┘                                           │                │
        ▲                                                     │ (frame being   │
        │                                                     │  rendered into │
        │ VSYNC callback:                                     │  this buffer)  │
        │ previous DISPLAYING                                 └───────┬────────┘
        │ buffer is freed                                             │
        │                                                             │ render complete +
        │                                                             │ cache flush
 ┌──────┴─────────┐                                                   │
 │  BUFFER_STATE_ │      VSYNC callback:                              ▼
 │  DISPLAYING    │ ◄──  PENDING promoted     ◄───────────── ┌────────────────┐
 │                │      to DISPLAYING                        │  BUFFER_STATE_ │
 │ (scanned out   │                                           │  PENDING       │
 │  to LCD by DMA)│                                           │                │
 └────────────────┘                                           │ (submitted to  │
                                                              │  DMA, waiting  │
                                                              │  for VSYNC)    │
                                                              └────────────────┘
```

### Render Task Loop (step by step)

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │                     DISPLAY RENDER TASK LOOP                        │
 │                                                                     │
 │  1. Check for mode change request                                   │
 │     (ANIMATION ↔ UI mode switch?)                                   │
 │         │                                                           │
 │  2. Wait on g_buffer_free_sem ──► acquire FREE buffer               │
 │     Mark buffer: FREE → RENDERING                                   │
 │         │                                                           │
 │  3. Call g_display_frame_callback(buffer, stride)                   │
 │     └─► Delegates to p3a_render_frame() (see §3)                   │
 │     Returns: frame_delay_ms, buffer_modified flag                   │
 │         │                                                           │
 │  4. Cache flush (esp_cache_msync) for DMA coherency                 │
 │         │                                                           │
 │  5. Wait for target frame time (if not max-speed mode)              │
 │         │                                                           │
 │  6. Wait for VSYNC semaphore (g_display_vsync_sem)                  │
 │         │                                                           │
 │  7. Mark buffer: RENDERING → PENDING                                │
 │     Store index in g_last_submitted_idx                             │
 │         │                                                           │
 │  8. Submit to DMA: esp_lcd_panel_draw_bitmap()                      │
 │         │                                                           │
 │  9. VSYNC interrupt fires (DMA complete):                           │
 │     ├─ PENDING → DISPLAYING (new frame is now on screen)            │
 │     ├─ Previous DISPLAYING → FREE                                   │
 │     └─ Signal g_buffer_free_sem (unblocks step 2)                   │
 │         │                                                           │
 │  └──── Loop back to step 1                                         │
 └─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Display Render Mode Switching

The display renderer supports two mutually exclusive modes:

```
 ┌────────────────────────────┐              ┌────────────────────────────┐
 │  DISPLAY_RENDER_MODE_      │              │  DISPLAY_RENDER_MODE_      │
 │  ANIMATION                 │              │  UI                        │
 │                            │              │                            │
 │  The render task loop      │  request +   │  µGFX library owns the     │
 │  calls the animation       │  wait for    │  frame buffers directly.   │
 │  frame callback each       │  acknowledge │  Used for provisioning,    │
 │  frame.                    │ ───────────► │  OTA progress, and         │
 │                            │              │  channel status messages.   │
 │  Used during normal        │ ◄─────────── │                            │
 │  playback and PICO-8       │  request +   │  Render task pauses and    │
 │  streaming.                │  wait for    │  yields buffer ownership.  │
 │                            │  acknowledge │                            │
 └────────────────────────────┘              └────────────────────────────┘

 Switching: display_renderer_enter_ui_mode()  / display_renderer_exit_ui_mode()
 Both block until the render task acknowledges the mode change.
```

---

## 3. Central Render Dispatcher (`p3a_render.c`)

The dispatcher is called every frame by the render task. It decides **what** to render based on the current application state.

```
 g_display_frame_callback(buffer, stride)
         │
         ▼
 ┌─────────────────────────────┐
 │  p3a_render_frame()         │
 │                             │
 │  Is boot logo active? ──────┼──► YES: render boot logo, return
 │         │                   │
 │         NO                  │
 │         │                   │
 │  Read p3a_state_get()       │
 │         │                   │
 │         ▼                   │
 │  ┌──────────────────────┐   │
 │  │ ANIMATION_PLAYBACK   │   │
 │  │                      │   │
 │  │  Substate?           │   │
 │  │  ├── PLAYING ────────┼───┼──► animation_player_render_frame_internal()
 │  │  │                   │   │       │
 │  │  │                   │   │       ├── Decode next native frame
 │  │  │                   │   │       ├── Parallel upscale (2 worker tasks)
 │  │  │                   │   │       │   via lookup tables (rotation + scale)
 │  │  │                   │   │       └── Write RGB565 to display buffer
 │  │  │                   │   │
 │  │  └── CHANNEL_MESSAGE ┼───┼──► ugfx_ui_render_to_buffer()
 │  │      (overlay text)  │   │       └── Render status text (fetching,
 │  └──────────────────────┘   │           downloading %, error, etc.)
 │                             │
 │  ┌──────────────────────┐   │
 │  │ PROVISIONING         ├───┼──► ugfx_ui_render_to_buffer()
 │  │                      │   │       └── Registration code, WiFi setup,
 │  │                      │   │           or status message
 │  └──────────────────────┘   │
 │                             │
 │  ┌──────────────────────┐   │
 │  │ OTA                  ├───┼──► ugfx_ui_render_to_buffer()
 │  │                      │   │       └── Progress bar + version info
 │  └──────────────────────┘   │
 │                             │
 │  ┌──────────────────────┐   │
 │  │ PICO8_STREAMING      ├───┼──► External render source writes
 │  │                      │   │    directly; return buffer_modified=false
 │  └──────────────────────┘   │
 └─────────────────────────────┘
```

---

## 4. Animation Player Pipeline

The animation player uses **double buffering** for seamless artwork swaps: the front buffer is being displayed while the back buffer loads the next animation.

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │                     ANIMATION PLAYER ARCHITECTURE                        │
 │                                                                          │
 │   ┌─────────────────────┐        ┌─────────────────────┐                │
 │   │    FRONT BUFFER      │        │    BACK BUFFER       │                │
 │   │                     │        │                     │                │
 │   │  Currently decoding  │        │  Loading next        │                │
 │   │  and rendering       │  SWAP  │  animation           │                │
 │   │  frames to display   │ ◄────► │  (async loader task) │                │
 │   │                     │        │                     │                │
 │   │  decoder instance    │        │  decoder instance    │                │
 │   │  native frame bufs   │        │  native frame bufs   │                │
 │   │  upscale LUTs        │        │  upscale LUTs        │                │
 │   └─────────────────────┘        └─────────────────────┘                │
 │                                                                          │
 │   Buffer Lifecycle:                                                      │
 │   LOAD ──► PREFETCH (decode 1st frame) ──► READY ──► SWAP to front      │
 └──────────────────────────────────────────────────────────────────────────┘
```

### Swap Process

```
 ┌──────────────────────┐
 │  Swap Trigger:        │
 │  • Play scheduler     │
 │    auto-swap timer    │
 │  • User tap (next)    │
 │  • Channel switch     │
 │  • Live mode schedule │
 └──────────┬───────────┘
            │
            ▼
 animation_player_request_swap(request)
            │
            ├── Set s_swap_requested = true
            │
            ▼
 ┌──────────────────────┐
 │  LOADER TASK          │  (separate FreeRTOS task)
 │                       │
 │  1. Open file / URL   │
 │  2. Create decoder    │
 │     (WebP/GIF/PNG/    │
 │      JPEG)            │
 │  3. Decode 1st frame  │
 │     into back buffer  │
 │     (prefetch)        │
 │  4. Mark back buffer  │
 │     first_frame_ready │
 └──────────┬───────────┘
            │
            ▼
 ┌──────────────────────┐
 │  RENDER CALLBACK      │  (called from display render task)
 │                       │
 │  1. Check: back       │
 │     buffer ready?     │
 │  2. YES: atomic swap  │
 │     front ↔ back      │
 │  3. Decode next frame │
 │     from front buffer │
 │  4. Parallel upscale  │
 │     to display buffer │
 │  5. Return frame      │
 │     delay (ms)        │
 └──────────────────────┘
```

### Frame Rendering Detail

```
 Native decoded frame (e.g. 128×128 RGB888)
         │
         ▼
 ┌─────────────────────────────┐
 │  Parallel Upscale            │
 │                             │
 │  Two FreeRTOS worker tasks   │
 │  split the frame vertically: │
 │                             │
 │  Worker 1: top half          │
 │  Worker 2: bottom half       │
 │                             │
 │  Each uses pre-computed      │
 │  lookup tables:              │
 │  • upscale_lookup_x[]        │
 │  • upscale_lookup_y[]        │
 │                             │
 │  These tables encode both    │
 │  nearest-neighbor upscaling  │
 │  and rotation in one step.   │
 └──────────────┬──────────────┘
                │
                ▼
 Display buffer (720×720 RGB565)
         │
         ▼
 DMA to LCD panel
```

---

## 5. Processing Notification Overlay

A small overlay triangle indicates background activity (e.g., swapping animations). This has its own mini state machine:

```
 ┌─────────────────┐  swap starts   ┌─────────────────┐
 │ PROC_NOTIF_     │ ──────────────►│ PROC_NOTIF_     │
 │ STATE_IDLE      │                │ STATE_PROCESSING │
 │                 │ ◄──────────────│ (blue triangle)  │
 │ (not visible)   │  swap succeeds └────────┬────────┘
 └─────────────────┘                         │ swap fails
                        ◄───────────────────┐│
                        3-second timeout    ││
                                            ▼│
                                     ┌─────────────────┐
                                     │ PROC_NOTIF_     │
                                     │ STATE_FAILED     │
                                     │ (red triangle,   │
                                     │  shown 3 seconds)│
                                     └─────────────────┘
```

---

## 6. Play Scheduler — Artwork Selection

The play scheduler is not a traditional state machine. It is a **command-driven engine** that determines which artwork to display next. It interacts with the animation player via swap requests.

```
 ┌───────────────────────────────────────────────────────────────────────┐
 │                      PLAY SCHEDULER FLOW                              │
 │                                                                       │
 │  Input: Playset (scheduler command)                                   │
 │  ├── Channel list (up to 64 channels)                                 │
 │  ├── Exposure mode: EQUAL / MANUAL / PROPORTIONAL                     │
 │  └── Per-channel pick mode: RECENCY / RANDOM                          │
 │                                                                       │
 │  Step 1: SELECT CHANNEL                                               │
 │  └── SWRR (Surplus Weight Round Robin)                                │
 │      Channels get turns based on normalized weights                   │
 │                                                                       │
 │  Step 2: PICK ARTWORK from selected channel                           │
 │  ├── RECENCY mode: cursor-based sequential walk                       │
 │  └── RANDOM mode: PCG32 PRNG selection                                │
 │                                                                       │
 │  Step 3: CHECK NAE POOL (recently shown)                              │
 │  └── 32-entry priority-weighted pool to avoid repeats                 │
 │                                                                       │
 │  Step 4: RECORD in history buffer                                     │
 │  └── 32-item circular buffer (enables back/forward navigation)        │
 │                                                                       │
 │  Step 5: REQUEST SWAP                                                 │
 │  └── animation_player_request_swap(artwork)                           │
 │                                                                       │
 │  Auto-swap: Timer fires after dwell time → triggers next swap         │
 └───────────────────────────────────────────────────────────────────────┘
```

---

## 7. End-to-End: From State Change to Pixels on Screen

```
 User action / system event
         │
         ▼
 p3a_state.c  (state transition + callback notification)
         │
         ▼
 p3a_render.c  (central render dispatcher, selects renderer)
         │
         ├──► animation_player  (decode + upscale)
         │         or
         ├──► ugfx_ui  (text/progress rendering)
         │         or
         └──► external source  (PICO-8 stream)
                  │
                  ▼
 display_renderer.c  (triple-buffered, VSYNC-synced)
         │
         ▼
 DMA → LCD Panel (720×720 IPS)
```
