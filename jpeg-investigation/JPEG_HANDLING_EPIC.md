# JPEG Handling Overhaul — Epic

**Status:** problem statement / investigation phase
**Owner:** TBD
**Created:** 2026-05-10
**Scope:** how p3a loads and decodes JPEG artworks on the ESP32-P4

---

## TL;DR

The JPEG handling on p3a is fragile in ways that are not surface-visible because:

1. The hardware JPEG decoder as wrapped by **ESP-IDF v5.5.1** has narrow input requirements (baseline only, source dimensions whose product is divisible by 8, dimensions that fit a multi-MB RGB buffer in PSRAM). The product-divisible-by-8 rule is an IDF-driver quirk in `esp_driver_jpeg/jpeg_parse_marker.c:106`, not an inherent hardware limit — the same check has been removed from upstream master / IDF v6.0.1.
2. The current p3a host code passes any `.jpg` file straight to that decoder with no SOF pre-validation and no software fallback path. (MCU output-buffer rounding *is* now handled — see Issue D status.)
3. As a result, a **substantial fraction of real-world JPEGs fail to decode**, but the failures are individually rare in normal playback because (a) most of the SD-card content is PNG/WebP/GIF, and (b) the recently-added silent-retry mechanism (3 retries before the loud overlay) masks short failure bursts.

The curated test corpus in `jpeg-investigation/animations/` (90 CC0-licensed JPEGs that exercise the failure modes) contains:

* **78 of 90 files (87%)** trip the IDF v5.5.1 `(W·H) % 8 != 0` SOF gate (Issue C). 89 of 90 have width not divisible by 8, but width-alone is not the driver predicate — see Issue C — and the 11-file gap between those two counts represents files that previously appeared "broken" but are now decodable on the HW path post-`732475d4` (Issue D, fixed). `160885.jpg` (664×893) remains the canonical working baseline.
* **30 files** that are progressive JPEGs → the HW decoder cannot parse them at all.
* **30 files** whose RGB output buffer would exceed 28 MiB → too large to allocate even from PSRAM.

In short: the device is currently lucky rather than correct on JPEG playback. This epic exists to make JPEG handling reliable and defensible.

---

## Background

* **Target hardware:** Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, 720×720 IPS panel.
* **Decoder used:** ESP-IDF's `esp_driver_jpeg` component, hardware-accelerated via the JPEG peripheral on ESP32-P4.
* **Wrapper:** `components/animation_decoder/` (JPEG path), invoked through `loader_service` and ultimately driven by the loader task in `main/animation_player_loader.c`.
* **Output format:** the decoder's RGB888 output is written to PSRAM-backed `native_frame_b1`/`b2` buffers, then upscaled into the 720×720 framebuffer.
* **Source content:** SD-card `animations/` folder is filled by users (uploads, file-copy) and by the Makapix Club vault (cached MQTT downloads). p3a does not control the encoder used to produce these JPEGs.

The ESP HW JPEG path, as it ships in **IDF v5.5.1**, has the following constraints (some are silicon-level, some are driver-level):

* **Baseline (SOF0) only** — the IDF driver does not parse progressive (SOF2), arithmetic-coded, lossless, or hierarchical JPEGs. Silicon-level: yes — the engine is a baseline-DCT decoder.
* **`(width × height) % 8 == 0` at SOF parse** — IDF v5.5.1's `jpeg_parse_marker.c:106` rejects any image whose dimensional product is not a multiple of 8. Driver-level only: this check is **absent from upstream master / IDF v6.0.1**, and authoritative Espressif documentation explicitly states there is no input-width divisibility requirement on the silicon — the codec simply pads its *output* to MCU boundaries.
* **MCU output padding** — the decoder writes the decoded pixels into a region rounded up to the MCU grid (8 for 4:4:4, 16 for 4:2:2 and 4:2:0). The caller must size the output buffer for the rounded dimensions; otherwise the driver returns `ESP_ERR_INVALID_ARG`. This is structural to the JPEG standard and is the same on every IDF version.
* **Output buffer must exist** — full-frame RGB888 must be allocated up-front in PSRAM; there is no streaming row mode in this wrapper.

p3a's current code (post-`732475d4`) handles MCU output padding correctly, but does **not** pre-validate SOF marker or `(W·H) % 8` before invoking the decoder.

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

This silent-retry behaviour is **valuable but not curative** — it hides bursts of two failures and masks the prevalence of the underlying problems. It is also at risk of being defeated as soon as the picker happens to land on three failing JPEGs in a row. With the corrected predicate, the curated corpus contains 30 progressive JPEGs (Issue B), 30 oversized JPEGs (Issue A), and 78 JPEGs that trip the IDF v5.5.1 SOF gate (Issue C) — the union covers 78 of 90 files, leaving only 12 decodable. In a uniform draw against this corpus the three-in-a-row failure probability is roughly (78/90)³ ≈ 65%; the curated set is biased toward failures by construction, but real folders with significant Makapix vault content are not far from this distribution.

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

### Issue C — `(width × height) % 8 != 0` (rejected at SOF parsing)

| | |
|---|---|
| **Log signature** | `E (xxxxx) jpeg.decoder: Picture sizes not divisible by 8 are not supported` → `jpeg_parse_marker(698): deal sof marker failed` → `ESP_ERR_INVALID_STATE` |
| **Root cause** | The IDF v5.5.1 driver source `components/esp_driver_jpeg/jpeg_parse_marker.c:106` contains `if ((width * height % 8) != 0) { ... return ESP_ERR_INVALID_STATE; }`. The check is on the **product** of the two dimensions, not on width alone. When violated, parsing fails before any allocation. The check is driver-level only and is absent from upstream master / IDF v6.0.1, which suggests the silicon does not need it. |
| **Files exercised** | 117032.jpg (697×900, W·H=627 300, %8=4), 94979.jpg (748×893, W·H=667 964, %8=4) |
| **Working baseline** | 160885.jpg (664×893): 664·893=593 752, %8=0 → passes. (Width 664 is divisible by 8 directly, so the product is too.) Height 893 is **not** divisible by 8; that's tolerated because the predicate is on the product. |
| **Predicate clarification** | A file with width%8=0 always passes (since 0·anything is divisible by 8). A file with height%8=0 always passes. A file with both odd dimensions almost always fails (odd·odd has at most one factor of 2). The widely-used "width divisible by 8" shorthand is roughly but not exactly correct. |

### Issue D — Output buffer sized for un-padded dimensions (FIXED in `732475d4`)

| | |
|---|---|
| **Status** | **Fixed** in commit `732475d4` (2026-05-07): the wrapper now rounds output dimensions up to 16 before allocating, and strips padding columns row-by-row when copying out. Files that pass Issue C's SOF gate but have width%16 ≠ 0 now decode successfully. Listed here for completeness and so the diagnostic still maps cleanly to historical logs. |
| **Original log signature** | `E (xxxxx) jpeg.decoder: Given buffer size 61440 is smaller than actual jpeg decode output size 1909248 the height and width of output picture size will be adjusted to 16 bytes aligned automatically` → `ESP_ERR_INVALID_ARG` |
| **Root cause** | Decoder auto-pads its output to a 16-byte MCU grid (for 4:2:0 / 4:2:2 sources) and refuses to write into a buffer sized for the un-padded dimensions. p3a originally allocated `width × height × 3`; the decoder demanded `aligned_w × aligned_h × 3`. |
| **Math** | 149410.jpg = 700×900. Decoder pads to 704×904. `704 × 904 × 3 = 1,909,248` — exact match for the log's "actual jpeg decode output size 1909248". |
| **Files exercised** | 149410.jpg (and any file that satisfies `(W·H) % 8 == 0` but has W%16 ≠ 0 or H%16 ≠ 0). |
| **Relationship to Issue C** | C and D were originally bucketed as "width not divisible by alignment, caught early vs late." With the v5.5.1 driver source in hand, the split is fully determined by `(W·H) % 8`: files where the product is not /8 are killed at SOF parse (C); files where it is /8 reach the decode stage and used to fail on buffer sizing (D, now fixed). Both share an underlying lineage — encoders padding to MCU at compress time — but they trip different gates and need different fixes. |

---

## Latent scope (test-corpus analysis)

`jpeg-investigation/inspect_jpegs.py` over all 90 JPEGs in the curated test corpus yields:

| Property | Count | Implication |
|----------|------:|-------------|
| No SOF marker (structurally broken) | 0 | Not a real failure mode in this set |
| Progressive (SOF2) | **30** | Will hit Issue B if picked |
| RGB buffer > 5 MB (W·H·3 > 5,000,000) | **30** | Will hit Issue A if picked |
| `(W·H) % 8 != 0` (IDF v5.5.1 SOF gate) | **78** | Will hit Issue C if picked |
| Width not divisible by 8 (informational) | 89 | Old shorthand; not the driver predicate. With Issue D fixed, the 11 files that have width%8 ≠ 0 but `(W·H) % 8 == 0` now decode successfully. |
| Height not divisible by 8 (informational) | 86 | Tolerated by HW decoder; not a failure source |

The "will-fail" sets overlap meaningfully (most of the giant 264xxx baseline JPEGs are in both set A and the `(W·H) % 8 != 0` set). Of the 90 files in the corpus, **12** now decode successfully on the HW path: the working baseline `160885.jpg` (664×893) plus the 11 files whose width is not divisible by 8 but whose `(W·H) % 8 == 0` — they previously tripped Issue D and now pass since `732475d4`.

The corpus was deliberately curated to exercise the failure modes; it is not a sample of typical traffic. In real-world use, the device's `/sdcard/p3a/animations/` folder is mixed media (PNGs, WebPs, GIFs alongside JPEGs), and the live Makapix vault has different content distribution. The numbers above describe the failure classes' *internal structure*, not the rate at which an end user encounters them.

### Why playback "mostly works" despite this

* Real-world SD-card content is mixed media — PNG, WebP, and GIF have separate decoders and do not share these failure modes. The picker is stochastic, so only a fraction of swaps target JPEGs.
* Commit `fa98ba7e` (silent-retry burst) absorbs up to two consecutive JPEG failures per cycle without showing an overlay.
* Users perceive the device as 24/7 reliable; the latent JPEG fragility surfaces only when staring at the monitor or when three failures land in sequence.

This is not a stable equilibrium — any change that increases JPEG share in the rotation (e.g. more Makapix art, more user uploads, a JPEG-heavy channel) will pierce it.

---

## Investigation artefacts

* `jpeg-investigation/inspect_jpegs.py` — Python helper that walks a folder of JPEGs and reports for each file: dimensions (PIL and raw-marker), file size, RGB-output size, SOF marker (SOF0 baseline vs SOF2 progressive), chroma subsampling, EXIF orientation, and width/height divisibility by 8 and 16. Prints a per-file row plus a summary block. Run with no args to scan `./animations/`.
* `jpeg-investigation/animations/` — curated test corpus of **90 CC0-licensed JPEGs** that collectively exercise every failure class catalogued below (plus the working baseline `160885.jpg` for control). Derived from a snapshot of the live SD card and trimmed down to JPEGs only, with all non-CC0 content removed. Safe to commit, redistribute, and use as a regression dataset.
* Original failure logs (excerpts) — captured in chat history; representative log lines reproduced in each issue row above.

---

## Decoder requirements summary (for designers of the fix)

| Constraint | Scope | Source |
|------------|-------|--------|
| Baseline (SOF0) only | Silicon | Issue B observations; ESP-IDF JPEG documentation |
| `(width × height) % 8 == 0` at SOF parse | **IDF v5.5.1 driver only** (removed in master / v6.0.1) | Issue C observations; `jpeg_parse_marker.c:106` |
| Neither width nor height alone needs to be divisible by anything specific | Silicon | 160885.jpg (664w/893h) success case; ESP-IDF JPEG documentation |
| Output buffer must be sized for **MCU-padded** dimensions (16 for 4:2:0/4:2:2, 8 for 4:4:4), not raw | Silicon (structural to JPEG) | Issue D observation; **handled in `732475d4`** |
| Single contiguous PSRAM allocation for full RGB888 frame | Wrapper design | Issue A observation; current decoder design |

---

## Recommended directions (not yet decisions)

These are options to weigh; no path has been chosen.

1. **Software JPEG fallback for HW-decoder-rejected files.**
   Detect SOF2 (Issue B) and `(W·H) % 8 != 0` (Issue C) before invoking the HW decoder, or catch `ESP_ERR_INVALID_STATE` from `jpeg_decoder_process` and re-route. Send those files to a software decoder (`libjpeg-turbo` or the much smaller `tjpgd`, or Espressif's `esp-new-jpeg`). One mechanism handles two of the three remaining issue classes (B and C).
   *Cost:* CPU time on decode, code-size for the SW decoder. Both decode-once at swap time, so user-perceived latency is the only concern — likely OK given the dwell time is seconds.
   *Risk:* memory pressure if SW decoder also allocates a full-frame buffer.

2. **Decode-time downscale for oversized images (Issue A).**
   ESP HW JPEG supports decode-time downscaling by 1/2, 1/4, 1/8 (need to verify on ESP32-P4 specifically). For a 3601×2701 source on a 720×720 display, decode at 1/4 → 900×675, RGB ≈ 1.8 MB. Easily fits. Note: per the authoritative Espressif report, the ESP32-P4 HW decoder has *no* built-in scaled-decode mode — downsampling has to happen post-decode via the on-chip PPA. That changes the calculus significantly: full-resolution intermediate still has to fit in PSRAM, so this option doesn't actually unlock the 28-MiB Issue A files. Confirm capabilities on v5.5.1 before banking on this path.
   *Cost:* if PPA-based, two passes (decode then scale) and a large transient buffer.
   *Risk:* may not be feasible at all for the 3601×2701 class.

3. **Pre-validation gate with eviction.**
   Reject unsupported JPEGs at index-build time rather than at decode time: scan SOF, dimensions, and `(W·H) % 8` when the SD-card cache is built, and either skip the file or transcode it server-side / on first download.
   *Cost:* longer index-build, still leaves the user with mysterious "missing" art unless paired with a clear UX.
   *Risk:* doesn't help Makapix MQTT-pushed art.

4. **Server-side normalisation.**
   For Makapix vault content, ensure the server only emits HW-decoder-friendly JPEGs (baseline, `(W·H) % 8 == 0`, capped resolution).
   *Cost:* server work, doesn't help SD-card user uploads.
   *Risk:* still need a device-side fallback for user-managed content.

5. **Hybrid: software fallback as the universal safety net + opportunistic HW path.**
   Keep the HW decoder as the fast path for files that satisfy its constraints (the inspect script's predicates, updated to `(W·H) % 8 == 0`, can be reused as a pre-check). Fall back to a software decoder for everything else. Combine with downscaling for Issue A files. This is the most robust direction; it's also the largest scope.

6. **ESP-IDF upgrade.**
   The Issue C SOF gate exists in the v5.5.1 driver and has been removed in upstream master / v6.0.1 (and likely earlier patch versions in the v5.5.x line). A bump to v5.5.2 or v5.5.3 may eliminate Issue C entirely without any p3a code changes — to verify, grep `components/esp_driver_jpeg/jpeg_parse_marker.c` in the candidate version for the `Picture sizes not divisible by 8` string. Cheapest possible win if it works.
   *Cost:* a normal IDF bump — toolchain re-install, regression-test the rest of the firmware against the new IDF.
   *Risk:* unrelated breakage from other components shifting under the IDF bump.

7. **Local IDF patch (stopgap).**
   Comment out or relax the four-line check at `jpeg_parse_marker.c:106-109` in a vendored IDF copy. The authoritative Espressif report indicates the silicon doesn't enforce the rule; the upstream master driver has already dropped the check, supporting that view.
   *Cost:* trivial code change.
   *Risk:* vendored-IDF patch has to be re-applied on every IDF bump, and there's a small chance the check is masking a real-but-rare hardware quirk that hasn't shown up in the upstream test suite. Acceptable as a release-blocker stopgap, not a maintainable solution.

---

## Open questions

* ~~What is the actual upper bound on width alignment — 8 (4:4:4 MCU) or 16 (4:2:0 MCU)?~~ **Resolved.** The IDF v5.5.1 driver enforces `(W·H) % 8 == 0` at SOF parse, regardless of chroma subsampling. The MCU output-padding (16 for 4:2:0/4:2:2, 8 for 4:4:4) is a separate, structural constraint and is now handled in `732475d4`. Per the authoritative Espressif report, the silicon itself imposes neither check on input width — both are wrapper/driver concerns.
* Does the ESP32-P4 HW JPEG peripheral support decode-time downscaling on IDF v5.5.1? The authoritative Espressif report indicates **no built-in scaled-decode mode** on the silicon — scaling has to happen post-decode via the PPA. Confirm whether the IDF API exposes any helper, and budget PSRAM for the full-resolution intermediate accordingly. Affects feasibility of recommendation (2).
* Is there an existing software JPEG decoder already pulled in by another component (e.g., LVGL), or would we be adding a new dependency? Influences code-size cost of recommendation (1).
* Does the IDF JPEG component expose a way to query supported features pre-decode, so we don't have to maintain our own marker-sniffing logic? `jpeg_decoder_get_info` already returns dimensions, so the `(W·H) % 8` and the SOF marker can be checked cheaply in user code; the open question is whether IDF will expose a pre-validation predicate as a documented API in a later version.
* Does IDF v5.5.2 (or any pre-v6 patch release) drop the `(W·H) % 8` check from `jpeg_parse_marker.c:106`? Determines whether recommendation (6) is the cheap path. Quick to verify by grep against a candidate IDF install.
* Should pre-validation happen at SD-card index time (paying it once per file) or at swap time (paying it on every play)? Trade-off between index latency and per-swap latency.

---

## Definition of done

The epic can be considered closed when:

* No JPEG file copied to `/sdcard/p3a/animations/` causes an on-screen "Failed to load artwork" overlay during normal auto-swap rotation, *or* the file is explicitly flagged as unsupported with a clear, non-error UI treatment.
* The full 90-file `jpeg-investigation/animations/` test corpus decodes (or is gracefully skipped) end-to-end without manual intervention.
* The silent-retry burst (`fa98ba7e`) is no longer the primary mechanism hiding JPEG failures — it remains as a safety net for genuinely broken files only.
* `jpeg-investigation/inspect_jpegs.py` continues to be a useful diagnostic and is referenced from the on-device docs.

---
