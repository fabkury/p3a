# CPU 1 Saturation Triggering IDLE1 Task Watchdog

Status: **(1) superseded 2026-05-15 by libjpeg progress monitor; (2)+ still deferred.** The scanline-loop yield landed on 2026-05-14 and was removed on 2026-05-15 in favor of a `jpeg_progress_mgr` callback that fires from inside libjpeg's Huffman/IDCT loops — see "Status update — 2026-05-15" below. The pinning recommendation (2) was *not* applied. The watchdog does not panic (`CONFIG_ESP_TASK_WDT_PANIC` unset), so any residual WDTs remain warnings rather than resets. 2026-06-07: the same slow decodes resurfaced as dwell-tick "swap already in progress" collisions — Tier 1 mitigation landed, see "Status update — 2026-06-07" below.

History: a second trace on 2026-05-14 reproduced the original `ycc_rgb_convert_internal` stack during V&A backfill and motivated (1). A third trace on 2026-05-15 caught a *progressive* JPEG stuck in `decode_mcu_AC_refine` — a case (1) couldn't cover — which motivated the progress-monitor replacement.

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

## Status update — 2026-05-14

### (1) Yield in SW JPEG scanline loop — IMPLEMENTED

`vTaskDelay(1)` every 32 scanlines, inside the `while (cinfo.output_scanline < cinfo.output_height)` loop in `jpeg_animation_decoder_sw.c`. With `CONFIG_FREERTOS_HZ=1000`, each yield is 1 ms; a 720-row image yields ~22 times, adding ~22 ms to a decode that already runs ~1.5 s on the worst case. Negligible cost, and IDLE1 now gets scheduled regularly enough to feed the WDT and run FreeRTOS housekeeping.

### (2) Pin `anim_loader` to CPU 0 — DEFERRED, observation-gated

Trade-offs that argued against pinning right now:

- `download_mgr` is already pinned to CPU 0 (`components/channel_manager/download_manager.c:836`). Museum SW decodes happen back-to-back with downloads, so pinning concentrates download (Wi-Fi/TLS crypto, high-priority kernel tasks) and decode on the same core. May slow decodes during active backfill.
- For static-image playback (very common with long swap intervals), CPU 1 sits mostly idle waiting for VSYNC. An unpinned `anim_loader` opportunistically uses those cycles; a pinned one cannot.
- With (1) in place, IDLE1 starvation is no longer the failure mode. The remaining motivation for pinning shrinks to "isolate the display pipeline on its own core" — a design call, not a bug fix.

**Revisit (2)** if post-deploy traces show:
- Any task watchdog firing (especially still `IDLE1` or `display_render`), or
- Visible display stutter that correlates with SW JPEG decode windows.

If both are absent, leaving `anim_loader` unpinned preserves FreeRTOS load balancing and the rationale above.

### Others (3)–(7)

Still as originally written. WebP-side yield (3) is the next likely candidate if any WDT remains and points at `display_render` in libwebp.

## Status update — 2026-05-15

### New variant: progressive JPEG inside `decode_mcu_AC_refine`

A WDT fired again on CPU 1 with `anim_loader` running:

```
E (1734526) task_wdt: Task watchdog got triggered. - IDLE1 (CPU 1)
E (1734526) task_wdt: CPU 1: anim_loader
MEPC : 0x480ccc12 — decode_mcu_AC_refine at jdphuff.c:577
RA   : 0x480ccd1a — decode_mcu_AC_refine at jdphuff.c:549

W (1746661) anim_player:    Swap request ignored: swap already in progress
W (1746662) ps_navigation:  Swap request failed: ESP_ERR_INVALID_STATE
```

`jdphuff.c` is libjpeg-turbo's progressive Huffman decoder. `decode_mcu_AC_refine` only runs for progressive AC refinement scans — baseline JPEGs use a different code path (`jdhuff.c`). The image was almost certainly the AIC pick made just before the WDT (`AIC · Drawing and Watercolor`, post 1261736080).

### Why the 2026-05-14 fix didn't cover it

The scanline-loop yield lived in the application's `while (cinfo.output_scanline < cinfo.output_height) { ... vTaskDelay(1) ... }`. For progressive JPEGs in single-pass mode (`cinfo.buffered_image == FALSE`, the default), libjpeg-turbo must consume **all scans** of the bitstream before any scanline can be output. The first call to `jpeg_read_scanlines` therefore blocks inside `decompress_data` → `decode_mcu_AC_refine` for the whole multi-scan decode, never returning to the loop where the yield lived. `output_scanline` stayed at 0 the entire time, and the yield never fired.

### (1') libjpeg progress monitor — IMPLEMENTED, supersedes (1)

Install `cinfo.progress = &mgr.pub` where `mgr.pub.progress_monitor` is a callback that yields on a wallclock cadence. libjpeg-turbo invokes the callback **once per MCU row** from inside `decompress_onepass` (baseline), `decompress_data` (progressive), and `consume_data` — i.e. from the same Huffman/IDCT call sites the WDT keeps catching. This covers progressive and baseline uniformly without depending on `output_scanline` advancing.

Implementation choices for low overhead:

- **Wallclock gate (`xTaskGetTickCount`), not call-count.** Yield frequency is constant in wallclock regardless of whether libjpeg fires the callback 100× or 1000× per image (progressive scan count varies wildly). Cost: one volatile read + subtract + compare per call.
- **200 ms interval.** Comfortably under the 15 s WDT and fast enough that `display_render` doesn't visibly stall waiting for CPU 1. Adds ~7 yields over a 1.5 s decode → ~0.5% overhead.
- **State in a wrapper struct (`p3a_progress_mgr_t`) with `struct jpeg_progress_mgr pub` first.** Standard libjpeg pattern, no globals, multi-instance safe.
- **Scanline-loop `vTaskDelay` removed.** The progress callback fires more often than every 32 scanlines for baseline *and* covers progressive, so the in-loop yield was strictly redundant.

Lives in `components/animation_decoder/jpeg_animation_decoder_sw.c`. Commit `07d91985`.

### (2) Pin `anim_loader` to CPU 0 — still deferred

The 2026-05-14 doc gated revisit on "any task watchdog firing." The 2026-05-15 WDT met that bar, but the root cause was a libjpeg coverage gap rather than core contention — (1') addresses it directly. Re-evaluate (2) only if a *new* WDT trace shows CPU 1 saturation that the progress monitor can't reach (e.g. libwebp, see (3)).

## Status update — 2026-06-07

### Silent sibling: dwell-tick "swap already in progress" collisions — Tier 1 landed

With (1') in place the WDT no longer fires, but the underlying 45–105 s SW JPEG decodes still hold the swap pipeline (`s_loader_busy` → `prefetch_pending`) across multiple 30 s dwell ticks. Each tick ran a full SWRR round + pick, then died in `animation_player_request_swap()`:

```
W anim_player:    Swap request ignored: swap already in progress
W ps_navigation:  Swap request failed: ESP_ERR_INVALID_STATE
```

Field log 2026-06-07: a `HAM · Oil` pick held the pipeline ~104 s (3 rejected ticks) and an `AIC · Painting` pick ~53 s (1 rejected tick); the display stayed frozen on the prior artwork throughout (confirmed by view-tracker "still watching" heartbeats for the same post across the window). The rejected picks debited SWRR credit with no rollback (`play_scheduler_next` only rolled back on `ESP_ERR_NOT_FOUND`).

Landed (Tier 1 — symptom + bookkeeping only, does not shorten the decode):

- `play_scheduler_timer.c`: the dwell tick is skipped while `animation_player_is_loader_busy()` (weak-linked; same predicate the rejection uses) — no pick, no SWRR round, no warning pair. User-initiated swaps (touch/REST) still reach the player and keep the loud rejection.
- `play_scheduler_navigation.c`: `play_scheduler_next()` now rolls back SWRR credits on any rejected fresh pick (non-OK results other than the already-handled `NOT_FOUND`), so a rejected attempt leaves the credit books as if the tick never happened.

Still open (Tier 2, deferred): bound the decode itself — wallclock budget in the SW JPEG progress monitor (abort via the existing setjmp/longjmp path; distinct error code, **exempt from corrupt-file deletion**) paired with a too-slow skip-list so the same image doesn't re-stall on every re-pick. A one-line per-SW-decode duration/dims/progressive log should land first to size the threshold and to check whether some hosts (e.g. HAM's NRS redirect) ignore the IIIF `!720,720` size hint. Proposals (4)/(5) above remain the source-level fixes.
