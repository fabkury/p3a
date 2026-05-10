# JPEG Handling Overhaul — Epic

**Status:** problem statement / investigation phase
**Owner:** TBD
**Created:** 2026-05-10
**Scope:** how p3a loads and decodes JPEG artworks on the ESP32-P4

---

## TL;DR

The JPEG handling on p3a is fragile in ways that are not surface-visible because:

1. The hardware JPEG decoder on ESP32-P4 has narrow input requirements (baseline only, width divisible by 8, dimensions that fit a multi-MB RGB buffer in PSRAM).
2. The current p3a host code passes any `.jpg` file straight to that decoder with no pre-validation, no fallback path, and no downscaling.
3. As a result, a **large majority of real-world JPEGs fail to decode**, but the failures are individually rare in normal playback because (a) most of the SD card content is PNG/WebP/GIF, and (b) the recently-added silent-retry mechanism (3 retries before the loud overlay) masks short failure bursts.

A representative test folder of 92 JPEGs (`jpeg-investigation/animations/`) contains:

* **90 of 92 files (98%)** with width not divisible by 8 → would trip the HW decoder's alignment guard if exercised.
* **30 files** that are progressive JPEGs → the HW decoder cannot parse them at all.
* **30 files** whose RGB output buffer would exceed 28 MiB → too large to allocate even from PSRAM.

In short: the device is currently lucky rather than correct on JPEG playback. This epic exists to make JPEG handling reliable and defensible.

---

## Background

* **Target hardware:** Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, 720×720 IPS panel.
* **Decoder used:** ESP-IDF's `esp_jpeg` component, hardware-accelerated via the JPEG peripheral on ESP32-P4.
* **Wrapper:** `components/animation_decoder/` (JPEG path), invoked through `loader_service` and ultimately driven by the loader task in `main/animation_player_loader.c`.
* **Output format:** the decoder's RGB888 output is written to PSRAM-backed `native_frame_b1`/`b2` buffers, then upscaled into the 720×720 framebuffer.
* **Source content:** SD-card `animations/` folder is filled by users (uploads, file-copy) and by the Makapix Club vault (cached MQTT downloads). p3a does not control the encoder used to produce these JPEGs.

The ESP HW JPEG decoder has well-known constraints:

* **Baseline (SOF0) only** — does not parse progressive (SOF2), arithmetic-coded, lossless, or hierarchical JPEGs.
* **MCU alignment** — the decode pipeline expects width to be a multiple of the MCU width (8 for 4:4:4 / 4:0:0; 16 for 4:2:2 and 4:2:0). When violated, the decoder either rejects at SOF parsing or auto-aligns the output and writes past the supplied buffer.
* **Output buffer must exist** — full-frame RGB888 must be allocated up-front; there is no streaming row mode in this wrapper.

p3a's current code does not check any of these properties before invoking the decoder.

---

## How the failure surfaces today (current behaviour)

The pipeline path on a JPEG load attempt:

```
play_scheduler_next()
  → prepare_and_request_swap()
  → animation_player_request_swap()
  → loader task: load_animation_into_buffer()
  → loader_service_load(filepath, ANIMATION_DECODER_TYPE_JPEG)
  → animation_decoder JPEG path
  → IDF jpeg_decoder
```

When the IDF decoder errors, the failure flows back up as `ESP_ERR_*`. Until commit `fa98ba7e`, every error rendered an on-screen `Failed to load artwork: <code>` overlay. After `fa98ba7e`, auto-swap (silent) failures retry up to 3 times before showing the overlay; user-initiated swaps still fail loudly. The overlay now also shows the basename of the failing file.

This silent-retry behaviour is **valuable but not curative** — it hides bursts of two failures and masks the prevalence of the underlying problems. It is also at risk of being defeated as soon as the picker happens to land on three misaligned-width JPEGs in a row (>50% probability with the current folder content; see "Latent scope" below).

---

## Issue catalogue

Four distinct failure classes have been observed in monitor logs and root-caused against the actual file properties via `jpeg-investigation/inspect_jpegs.py`. The math in each row is exact: decoded buffer sizes match the log byte counts to the byte, confirming the diagnosis.

### Issue A — RGB buffer too large to allocate

| | |
|---|---|
| **Log signature** | `E (xxxxx) jpeg_decoder: Failed to allocate RGB buffer (29332992 bytes)` → `ESP_ERR_NO_MEM` |
| **Root cause** | Source JPEG dimensions (3601×2701, ~9.7 MP) require ~28 MiB of contiguous PSRAM for the RGB888 output buffer. Allocation fails. |
| **Math** | The HW decoder pads to 16-byte alignment: 3601→3616, 2701→2704. `3616 × 2704 × 3 = 29,332,992` — exact match for the log byte count. |
| **Files exercised** | 264986.jpg, 264902.jpg, 264901.jpg, 264988.jpg, 264984.jpg |
| **All matching files in test set (30)** | 262131, 262135, 264896, 264901, 264902, 264909, 264912, 264914, 264917, 264923, 264924, 264925, 264927, 264979, 264980, 264981, 264982, 264983, 264984, 264985, 264986, 264987, 264988, 264989, 264990, 264991, 264992, 264994, 264995, 264996 — all uniformly 3601×2701 |
| **Likely origin** | Makapix Club server-side art at "press kit" resolution rather than the device's display resolution. |

### Issue B — Progressive JPEG (SOF2) rejected

| | |
|---|---|
| **Log signature** | `E (xxxxx) jpeg_decoder: Invalid JPEG dimensions: 0 x 0` → `ESP_ERR_INVALID_SIZE` |
| **Root cause** | The ESP HW JPEG decoder supports only baseline (SOF0). It cannot parse SOF2's interleaved scans, so it never extracts the dimensions and reports 0×0. |
| **Math / verification** | `inspect_jpegs.py` raw-marker scan confirms `SOF2` for both observed files. PIL likewise reports `progressive=True`. |
| **Files exercised** | 138630.jpg (843×586), 61560.jpg (843×1132) |
| **All matching files in test set (30)** | 132519, 138629, 138630, 138636, 138652, 53973, 53976, 53985, 54038, 54190, 54263, 54266, 54313, 54363, 59485, 59487, 59489, 59491, 59506, 59521, 59523, 59541, 61545, 61548, 61552, 61555, 61558, 61560, 61563, 61565 |
| **Likely origin** | Web-optimised JPEGs encoded with progressive scan (common for assets served by image CDNs that target browser rendering). |

### Issue C — Width not divisible by 8 (rejected at SOF parsing)

| | |
|---|---|
| **Log signature** | `E (xxxxx) jpeg.decoder: Picture sizes not divisible by 8 are not supported` → `jpeg_parse_marker(698): deal sof marker failed` → `ESP_ERR_INVALID_STATE` |
| **Root cause** | HW decoder requires the source width to be divisible by the MCU width (8 for 4:4:4, 16 for 4:2:0/4:2:2). When violated, parsing fails before any allocation. |
| **Files exercised** | 117032.jpg (697×900, w%8=1), 94979.jpg (748×893, w%8=4) |
| **Working baseline confirms width-only constraint** | 160885.jpg (664×893) decodes successfully. Width 664 is divisible by 8 (664/8=83); height 893 is **not** divisible by 8 (893/8=111.625). The HW decoder accepts misaligned height but not misaligned width. |

### Issue D — Width not divisible by alignment (rejected at decode-time)

| | |
|---|---|
| **Log signature** | `E (xxxxx) jpeg.decoder: Given buffer size 61440 is smaller than actual jpeg decode output size 1909248 the height and width of output picture size will be adjusted to 16 bytes aligned automatically` → `ESP_ERR_INVALID_ARG` |
| **Root cause** | Same width-alignment violation as Issue C, but caught later: the decoder accepts the SOF, auto-pads dimensions to alignment for the output stage, and then notices that the supplied output buffer is sized for the un-padded dimensions and would overflow. |
| **Math** | 149410.jpg = 700×900. Decoder pads to 704×904. `704 × 904 × 3 = 1,909,248` — exact match for the log's "actual jpeg decode output size 1909248". |
| **Files exercised** | 149410.jpg |
| **Why C vs D for similarly-misaligned files** | Unclear from file properties alone. Width parity, height, and width%16 do not separate the two C files (697w odd, 748w even) from the one D file (700w even). The split likely depends on internal IDF state — chroma subsampling, JFIF/EXIF marker presence, or the order of internal validation steps. **For mitigation purposes, C and D are the same root cause.** |
| **All matching files in test set (combined C+D)** | **90 of 92 JPEGs** in the folder have a width not divisible by 8. Only two files satisfy the alignment constraint: `160885.jpg` (664×893) and `iran-news-map-0314-promo-untitled-square640.jpg` (640×640). Effectively **virtually every JPEG in the folder is at risk** of C or D if exercised. |

---

## Latent scope (full-folder analysis)

`jpeg-investigation/inspect_jpegs.py` over all 92 JPEGs in the test folder yields:

| Property | Count | Implication |
|----------|------:|-------------|
| No SOF marker (structurally broken) | 0 | Not a real failure mode in this set |
| Progressive (SOF2) | **30** | Will hit Issue B if picked |
| RGB buffer > 5 MB (W·H·3 > 5,000,000) | **30** | Will hit Issue A if picked |
| Width not divisible by 8 | **90** | Will hit Issue C or D if picked |
| Height not divisible by 8 | 87 | Tolerated by HW decoder; not a failure source |

The three "will-fail" sets overlap meaningfully (most of the giant 264xxx baseline JPEGs are in A and also in width%8≠0 i.e. C/D set). On the test folder, only **a handful** of JPEGs would decode successfully on the HW decoder as-is — at least one confirmed from the logs (`160885.jpg`, 664×893).

### Why playback "mostly works" despite this

* The folder is mixed media: 92 JPEGs but also 29 PNGs and 1 WebP. PNG/WebP have different decoders that don't share these failure modes.
* The picker is stochastic — across thousands of swaps, only a fraction land on JPEGs.
* Commit `fa98ba7e` (silent-retry burst) absorbs up to two consecutive JPEG failures per cycle without showing an overlay.
* Users perceive the device as 24/7 reliable; the latent JPEG fragility surfaces only when staring at the monitor or when three failures land in sequence.

This is not a stable equilibrium — any change that increases JPEG share in the rotation (e.g. more Makapix art, more user uploads, a JPEG-heavy channel) will pierce it.

---

## Investigation artefacts

* `jpeg-investigation/inspect_jpegs.py` — Python helper that walks a folder of JPEGs and reports for each file: dimensions (PIL and raw-marker), file size, RGB-output size, SOF marker (SOF0 baseline vs SOF2 progressive), chroma subsampling, EXIF orientation, and width/height divisibility by 8 and 16. Prints a per-file row plus a summary block. Run with no args to scan `./animations/`.
* `jpeg-investigation/animations/` — exact copy of the SD-card `animations/` folder used during testing (121 files: 92 JPEG, 29 PNG, 1 WebP, 1 metadata file).
* Original failure logs (excerpts) — captured in chat history; representative log lines reproduced in each issue row above.

---

## Decoder requirements summary (for designers of the fix)

| Constraint | Source |
|------------|--------|
| Baseline (SOF0) only | Issue B observations |
| Width divisible by 8 | Issue C/D observations + 160885.jpg success case |
| Height alignment is **not** required | 160885.jpg (893h) success case |
| Output buffer must be sized for **aligned** dimensions, not raw | Issue D observation |
| Single contiguous PSRAM allocation for full RGB888 frame | Issue A observation; current decoder design |

---

## Recommended directions (not yet decisions)

These are options to weigh; no path has been chosen.

1. **Software JPEG fallback for HW-decoder-rejected files.**
   Detect SOF2 (Issue B) and width%8≠0 (Issue C/D) before invoking the HW decoder. Route those files to a software decoder (`libjpeg-turbo` or the much smaller `tjpgd`). One mechanism handles three issue classes.
   *Cost:* CPU time on decode, code-size for the SW decoder. Both decode-once at swap time, so user-perceived latency is the only concern — likely OK given the dwell time is seconds.
   *Risk:* memory pressure if SW decoder also allocates a full-frame buffer.

2. **Decode-time downscale for oversized images (Issue A).**
   ESP HW JPEG supports decode-time downscaling by 1/2, 1/4, 1/8 (need to verify on ESP32-P4 specifically). For a 3601×2701 source on a 720×720 display, decode at 1/4 → 900×675, RGB ≈ 1.8 MB. Easily fits.
   *Cost:* none beyond the API call; output is a different size, so the upscale-map code path still needs to handle it.
   *Risk:* if the HW decoder's downscale doesn't accept misaligned widths, this only helps the well-aligned giant images, not all of Issue A.

3. **Pre-validation gate with eviction.**
   Reject unsupported JPEGs at index-build time rather than at decode time: scan SOF, dimensions, alignment when the SD-card cache is built, and either skip the file or transcode it server-side / on first download.
   *Cost:* longer index-build, still leaves the user with mysterious "missing" art unless paired with a clear UX.
   *Risk:* doesn't help Makapix MQTT-pushed art.

4. **Server-side normalisation.**
   For Makapix vault content, ensure the server only emits HW-decoder-friendly JPEGs (baseline, width%8=0, capped resolution).
   *Cost:* server work, doesn't help SD-card user uploads.
   *Risk:* still need a device-side fallback for user-managed content.

5. **Hybrid: software fallback as the universal safety net + opportunistic HW path.**
   Keep the HW decoder as the fast path for files that satisfy its constraints (the inspect script's predicates can be reused as a pre-check). Fall back to a software decoder for everything else. Combine with downscaling for Issue A files. This is the most robust direction; it's also the largest scope.

---

## Open questions

* Does the ESP32-P4 HW JPEG peripheral support decode-time downscaling, and via what API in the IDF version we use (5.5.x)? Affects feasibility of (2).
* What is the actual upper bound on width alignment — 8 (4:4:4 MCU) or 16 (4:2:0 MCU)? The successful baseline (664-wide, divisible by 8 but not 16) suggests 8 is sufficient at least for some chroma subsamplings. Need to test a 4:2:0 file with width%8=0 but width%16≠0 to confirm.
* Is there an existing software JPEG decoder already pulled in by another component (e.g., LVGL), or would we be adding a new dependency? Influences code size cost.
* Does the IDF JPEG component expose a way to query supported features pre-decode, so we don't have to maintain our own marker-sniffing logic?
* Should pre-validation happen at SD-card index time (paying it once per file) or at swap time (paying it on every play)? Trade-off between index latency and per-swap latency.

---

## Definition of done

The epic can be considered closed when:

* No JPEG file copied to `/sdcard/p3a/animations/` causes an on-screen "Failed to load artwork" overlay during normal auto-swap rotation, *or* the file is explicitly flagged as unsupported with a clear, non-error UI treatment.
* The full 92-file `jpeg-investigation/animations/` test set decodes (or is gracefully skipped) end-to-end without manual intervention.
* The silent-retry burst (`fa98ba7e`) is no longer the primary mechanism hiding JPEG failures — it remains as a safety net for genuinely broken files only.
* `jpeg-investigation/inspect_jpegs.py` continues to be a useful diagnostic and is referenced from the on-device docs.

---
