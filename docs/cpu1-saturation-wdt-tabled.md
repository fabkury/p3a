# CPU 1 Saturation Triggering IDLE1 Task Watchdog

Status: **Diagnosed, fix tabled.** Captured from a live session on 2026-05-12. The watchdog does not panic (`CONFIG_ESP_TASK_WDT_PANIC` unset), so the device keeps running — these are warnings, but they're a signal that CPU 1 is being held for >15 s by non-yielding decode work. Pick this up when ready to address.

## Symptom

Three consecutive Task Watchdog Timer firings on `IDLE1 (CPU 1)`, ~15 s apart, while playback continued. Excerpted from the log:

```
E (2172509) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (2172509) task_wdt:  - IDLE1 (CPU 1)
E (2172509) task_wdt: CPU 0: IDLE0
E (2172509) task_wdt: CPU 1: display_render
MEPC : 0x4809bf9a  --- VP8LAddGreenToBlueAndRed_C at libwebp/dsp/lossless.c:265
RA   : 0x4809c85a  --- VP8LInverseTransform at libwebp/dsp/lossless.c:400

E (2187509) task_wdt: ... IDLE1 (CPU 1)
E (2187509) task_wdt: CPU 0: upscale_top
E (2187509) task_wdt: CPU 1: anim_loader
MEPC : 0x480d91fe  --- jpeg_idct_islow at libjpeg-turbo/jidctint.c:401
RA   : 0x480cab32  --- decompress_data at libjpeg-turbo/jdcoefct.c:313

E (2202509) task_wdt: ... IDLE1 (CPU 1)
E (2202509) task_wdt: CPU 0: IDLE0
E (2202509) task_wdt: CPU 1: upscale_bottom
MEPC : 0x480ce2c0  --- ycc_rgb_convert at libjpeg-turbo/jdcolor.c:295
RA   : 0x480c219e  --- sep_upsample at libjpeg-turbo/jdsample.c:106

W (2186209) anim_player:    Swap request ignored: swap already in progress
W (2186209) ps_navigation:  Swap request failed: ESP_ERR_INVALID_STATE
```

The AIC pick at t=2156184 (post 991558443, 999-entry pool) didn't finish decoding until t=2204795 — **~48 s on a single image**, fully on the slow path.

## Diagnosis

CPU 1 is saturated by non-yielding CPU-bound decoders, made worse by a task layout that lets all of them land on the same core.

### Task layout (CPU 1 hot)

| Task | Pin | Priority | Source |
|------|-----|----------|--------|
| `display_render` | CPU 1 | 8 | `main/display_renderer.c:270` |
| `upscale_bottom` | CPU 1 | 8 | `main/display_renderer.c:197` |
| `upscale_top` | CPU 0 | 8 | `main/display_renderer.c:184` |
| `anim_loader` | **unpinned** | 7 | `main/animation_player.c:341` |

`anim_loader` being unpinned means the FreeRTOS scheduler can drop it onto CPU 1 whenever it is the highest-priority ready task there. Combined with the SW JPEG path's lack of yields, this means one museum image can hold CPU 1 for tens of seconds.

### Non-yielding hot loops the WDT caught

1. **WDT #1** — `display_render` in `VP8LAddGreenToBlueAndRed_C` → animated **lossless WebP** per-frame decode. libwebp does not yield; `grep vTaskDelay main/display_renderer.c` returns 0 hits.
2. **WDT #2** — `anim_loader` in `jpeg_idct_islow`. The SW JPEG path at `components/animation_decoder/jpeg_animation_decoder_sw.c:161-173` runs the entire scanline loop with **no `vTaskDelay`, no `taskYIELD`, no `esp_task_wdt_reset`**. The whole `components/animation_decoder/` tree has 0 yield calls. No task in the codebase subscribes to the WDT (`grep esp_task_wdt_reset` returns nothing).
3. **WDT #3** — `upscale_bottom` in `ycc_rgb_convert`. Same SW JPEG decode still in progress on CPU 1; the task that happens to be on the core at the moment of the WDT check changed.

### Why the SW path is hit so often

AIC IIIF URLs are built `!720,720` (`components/art_institution/museums/artic.c:88`), which preserves aspect ratio. The server returns whatever dimensions match the source — e.g. 720×703, 720×494, 478×720. Per the header comment in `jpeg_animation_decoder_sw.c:8-12`, the IDF 5.5.1 HW JPEG decoder rejects files where individual dims aren't multiples of 8. 703, 494, 478 → all rejected → SW fallback. V&A 720×480 hits HW (480 % 8 == 0); 627×720 falls back (627 not %8).

### Why it surfaced now, not before

Pre-Museums, the rotation was Giphy/Makapix WebP and GIFs — small frames, mostly HW-eligible. AIC (999-entry pool) + V&A landed in the rotation, and most of those images now hit the slow path. When the SW decode of one of them coincides with the on-screen animation being a lossless WebP (display_render busy on CPU 1), nothing on CPU 1 yields long enough for IDLE1 to feed the WDT.

### Relevant config

```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=15
# CONFIG_ESP_TASK_WDT_PANIC is not set
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y
CONFIG_P3A_RENDER_TASK_PRIORITY=8  (anim_loader runs at PRIORITY - 1 = 7)
```

The WDT does not panic — that's the only reason the device keeps running through this. If `CONFIG_ESP_TASK_WDT_PANIC` is ever flipped on, these warnings become resets.

## Proposed solutions (ranked)

### Highest leverage, smallest blast radius

1. **Yield inside the SW JPEG scanline loop.** `components/animation_decoder/jpeg_animation_decoder_sw.c:161-173`: call `vTaskDelay(1)` (or `esp_task_wdt_reset()` after subscribing) every 16 or 32 scanlines. Single-handedly prevents WDTs #2 and #3. Negligible perf hit at the 1 ms FreeRTOS tick.

2. **Pin `anim_loader` to CPU 0.** `main/animation_player.c:341` — switch `xTaskCreate` to `xTaskCreatePinnedToCore(..., 0)`. Keeps SW-decode work off the core where `display_render` and `upscale_bottom` are pinned. With (1), this leaves CPU 1 drainable.

### Address the WebP arm of the saturation

3. **Yield inside the per-frame WebP decode path in `display_render`** (or wherever `WebPIDecode*` is invoked). One `vTaskDelay(1)` per frame in the render task is enough to prevent WDT #1.

### Cut SW-fallback frequency at the source

4. **Request HW-friendly dimensions from museums.** Probe the AIC response and re-request rounded-down-to-multiple-of-8, or use a different IIIF size syntax that constrains a single known-good axis. Won't help every awkward aspect, but cuts a meaningful share.

5. **Skip-blacklist HW-rejected JPEGs.** Log + persist, so the same post doesn't keep falling back to SW on every reboot.

### Symptom-only / defensive (do not pair alone)

6. Bump `CONFIG_ESP_TASK_WDT_TIMEOUT_S` from 15 → 30. Hides the warning without fixing the pile-up. Pairs poorly with any future work that wants the WDT to mean something.

7. Subscribe `display_render`, `upscale_*`, `anim_loader` to the WDT via `esp_task_wdt_add` and call `esp_task_wdt_reset()` in their loops. More invasive but makes "did this task make progress" a first-class signal.

## Recommendation

Start with **(1) + (2)**. Five-line changes each, directly target the observed call stacks, no new infrastructure. **(3)** is the next one to check once (1)+(2) silence the JPEG-side WDTs and any remaining WebP-side WDT becomes the dominant signal. **(4)** is a deeper investment but also buys throughput and SD-cache hit rate, not just WDT silence.

## Related

- [refresh-lai-race.md](refresh-lai-race.md) — another concurrency edge in the play scheduler path.
- [art-institutions/finalized-design.md](art-institutions/finalized-design.md) — museum URL build and rate-limit design.
