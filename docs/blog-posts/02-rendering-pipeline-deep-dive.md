# Pushing Pixels: Building a 720x720 Animation Pipeline on ESP32-P4

Rendering smooth animations on a microcontroller is a satisfying engineering problem. You're working with real constraints — limited memory, no GPU in the traditional sense, and a display that expects a steady stream of pixel data via DMA. When we built p3a's rendering pipeline, every buffer allocation and every frame timing decision mattered. Here's how we put it together.

## The Display Hardware

p3a drives a 720x720 24-bit IPS panel through MIPI-DSI, controlled by an ST7703 display controller. At RGB888, a single frame is 720 x 720 x 3 = **1,555,200 bytes** — about 1.5 MB. That's a lot of memory on a microcontroller, and we need multiple buffers.

We use **triple buffering** with all frame buffers allocated in PSRAM. One buffer is being scanned out by the DMA controller, one holds the completed previous frame, and one is being written to by the renderer. A VSYNC interrupt signals when the display has finished scanning a frame, and we rotate the buffers. This eliminates tearing completely — the DMA never reads from a buffer that's still being written to.

## The Decode-Render-Display Pipeline

Every frame travels through a three-stage pipeline:

```
Decoder → Composition/Scaling → Display
```

### Stage 1: Decode

p3a supports four image formats, each with its own decoder:

- **Animated WebP** — our recommended format. Good compression, supports both lossy and lossless, handles transparency with a full alpha channel.
- **GIF** — the classic. A C++ decoder wrapper handles the LZW decompression and frame disposal methods.
- **PNG** — static images with full alpha support.
- **JPEG** — static images, no transparency.

The decoders produce raw pixel data at the artwork's native resolution. A 32x32 pixel art sprite stays 32x32 at this stage. A 1080x1080 photograph stays at 1080x1080. The scaling comes next.

### Stage 2: Composition and Scaling

This is where things get interesting. The composition stage handles three problems simultaneously:

**Aspect ratio preservation.** Artwork comes in all shapes — square, portrait, landscape. We calculate the largest size that fits within 720x720 while maintaining the original proportions. A 128x96 image becomes 720x540, centered on the display with the background color filling the remaining space.

**Transparency compositing.** Images with alpha channels are blended over a configurable background color. The background defaults to black but can be changed through the web UI or REST API. This means transparent pixel art sprites look great regardless of their original background assumptions.

**Smart upscaling.** This is the decision we're proudest of. We route frames through different scaling algorithms based on the content source:

- **Pixel art** (Makapix and local files) goes through **CPU nearest-neighbor scaling**. This preserves the crisp, hard edges that define pixel art. A 64x64 sprite scaled to 720x720 looks exactly like the original, just bigger — every pixel is a clean rectangle with no blurring.

- **Giphy content** (photographs, video clips, screen recordings) goes through **PPA hardware bilinear interpolation**. The ESP32-P4's Pixel Processing Accelerator has a Scale-Rotate-Mirror (SRM) engine that performs bilinear interpolation in hardware. Smooth gradients and photographic detail stay smooth after upscaling.

This content-aware routing happens automatically based on the channel the artwork belongs to.

### CPU Nearest-Neighbor: Parallel by Design

Our CPU upscaler splits the work across **two FreeRTOS tasks**, each responsible for half the output frame (top and bottom). This halves the scaling time on the dual-core ESP32-P4.

Both tasks handle all four rotation orientations (0, 90, 180, 270 degrees) and fill border regions with the background color. There's careful cache coherency management since the source and destination buffers live in PSRAM — we flush and invalidate cache lines at the right boundaries to avoid visual corruption.

### PPA Hardware Bilinear: Let the Silicon Work

The PPA path offloads scaling entirely to dedicated hardware. We configure the SRM engine with the source buffer, destination buffer, scaling factors, and rotation angle, then let it rip. The PPA also handles R/B channel swapping (the decoder outputs RGB, the display expects BGR in some configurations) as part of the same operation — zero extra cost.

Border regions are filled using the PPA's Fill operation, which writes a solid color rectangle at hardware speed.

## Overlays

On top of the rendered frame, we composite lightweight overlays:

**FPS counter** — a tiny 5x7 bitmap font (2x scaled) in the top-right corner. Useful during development, togglable via settings. It tracks real frame timing, not theoretical rates.

**Processing notification** — a small triangle indicator in the bottom-right corner. Blue checkerboard pattern while the next animation is being loaded and decoded; red if loading fails. It's a state machine: IDLE → PROCESSING → FAILED → IDLE, with a 5-second timeout on processing and 3-second display on failure. Subtle enough that you barely notice it, informative enough that you know the device is working.

## Rotation

Screen rotation (0, 90, 180, 270 degrees) is handled throughout the pipeline, not bolted on at the end. Both the CPU and PPA upscalers natively support all four orientations. The rotation is triggered by a two-finger gesture on the touchscreen and persists across reboots via NVS flash storage.

This matters because rotating a 1.5 MB frame buffer after rendering would be expensive. By integrating rotation into the scaling step, we do it for free — the upscaler just reads source pixels in a different order.

## Frame Timing

The animation player manages frame timing based on each frame's specified delay. For animated WebPs and GIFs, each frame can have a different duration. The player tracks elapsed time and advances frames accordingly, requesting new decode-render cycles from the pipeline.

Between animations, there's an auto-advance timer (default 30 seconds, configurable) that triggers the next artwork in the playlist. Touch interaction resets this timer.

## Memory Budget

On an ESP32-P4 with PSRAM, memory is generous by microcontroller standards but still finite. Our major allocations:

- **3 frame buffers**: ~4.7 MB (720x720x3 bytes x 3)
- **Decode buffers**: Variable, depends on artwork resolution
- **SD card cache**: Bounded by configurable limits per channel

The triple-buffer scheme is the biggest single allocation. We considered double buffering to save 1.5 MB, but the elimination of tearing and the decoupling of render and display timing was worth the cost.

## What We Learned

Building this pipeline reinforced a few principles:

**Match the algorithm to the content.** Nearest-neighbor and bilinear interpolation are both "correct" — for different types of images. Choosing automatically based on content source gave us the best results without user configuration.

**Integrate transformations early.** Rotation, scaling, and color space conversion are cheapest when combined into a single pass. The PPA does all three simultaneously.

**Triple buffering is worth it.** The memory cost is real, but the elimination of tearing and the timing flexibility it provides made the pipeline dramatically simpler to reason about.

**Use the hardware.** The ESP32-P4's PPA is a hidden gem. Not many projects use it yet, but for image processing on this chip, it's the difference between "possible" and "smooth."

The result is a pipeline that plays animated pixel art at full frame rate with zero tearing on a 720x720 display, all on a microcontroller that costs a few dollars. Not bad for a pixel art player.
