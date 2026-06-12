# Intro-Animations — Architecture

How 12 animations share one codebase across firmware and a Windows host
harness. Phase tracking is in [plan.md](plan.md); this file is the technical
design.

## Current code (baseline, verified 2026-06-11)

- `components/p3a_core/p3a_boot_logo.c` — the whole current animation: reads
  `esp_timer_get_time()`, computes phase (250 ms delay / 2000 ms smoothstep
  fade / 1000 ms hold), clears the buffer to bg color, blits the logo.
  Returns 40 ms as the next-frame delay; returns -1 when expired.
- `components/p3a_core/p3a_logo.c` — 46×54 BGR888 logo with 0x808080 chroma
  key + `p3a_logo_blit_pixelwise_bgr888()` (alpha, scale, rotation,
  clipping). **Already pure C** (only stdint) — compiles on host unchanged.
- `components/p3a_core/p3a_render.c:144` — calls the boot logo with highest
  priority; the returned delay paces the render task
  (`main/animation_player.c:92`).
- Buffer: BGR888, 720×720, stride passed in (~1.48 MB/frame), framebuffer
  in PSRAM.

The animation code is already decoupled from LCD/DMA/VSYNC machinery — it
just writes into `(buffer, w, h, stride)`. That's what makes host
development cheap.

## Target layout

```
components/p3a_core/
  p3a_boot_logo.c            # becomes the intro MANAGER (device-only):
                             #   clock, blank-delay + hold bookends, normalized t,
                             #   duration from NVS, seed from esp_random(),
                             #   random pick + force-override, INFO pick log.
                             #   Public API unchanged: init / is_active / render.
  p3a_logo.c                 # unchanged (pure, shared)
  intro_anims/               # SHARED, pure C, zero ESP-IDF includes
    intro_anim.h             # interface + context + registry declaration
    intro_anim_registry.c    # registry array (name -> fns), count
    ia_smoothstep_fade.c     # animation #1 (port of current behavior)
    ia_<name>.c              # one file per animation, prefix "ia_"

host/intro-anim-lab/         # PERMANENT host harness (Windows)
  build.ps1                  # gcc invocation; no cmake/ninja
  README.md                  # usage: keys, CLI flags, check mode
  main.c                     # CLI parsing, mode dispatch
  viewer_win32.c             # GDI window (StretchDIBits)
  dump.c                     # BMP frame dump + optional ffmpeg assembly
  checks.c                   # automated contract suite
```

Firmware compiles `intro_anims/*.c` via `p3a_core`'s CMakeLists. The host
build compiles the same files plus `p3a_logo.c` with `-I` pointing at the
component's include dirs. **The host build doubles as the purity proof**:
if someone adds an `esp_log.h` include to an animation module, the host
build breaks.

## Animation interface (sketch — finalize in Phase 2)

```c
typedef struct {
    int      width, height;      // 720, 720
    size_t   stride;             // bytes per row
    uint8_t  bg_r, bg_g, bg_b;   // user-configured background color
    uint16_t rotation;           // 0 / 90 / 180 / 270
    int      logo_x, logo_y;     // precomputed centered position at scale
    int      logo_scale;         // 3
    uint32_t seed;               // per-boot seed; sole randomness source
} intro_anim_ctx_t;

typedef struct {
    const char *name;             // kebab-case, e.g. "smoothstep-fade"
    int         frame_budget_ms;  // animation's declared per-frame budget,
                                  //   used by both the device manager and the
                                  //   host viewer to pace playback.
                                  //   Default 33 (30 FPS). Animations may
                                  //   request a different budget if needed.
    void (*render)(uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
} intro_anim_t;

extern const intro_anim_t intro_anim_registry[];
extern const int intro_anim_count;
```

Rules for `render()`:

- Output for a given `(ctx, t)` must be **bit-exact deterministic** — no
  statics persisting across frames, no wall clock, no global RNG.
  Randomness = small inline PRNG (e.g. splitmix32/xorshift) seeded from
  `ctx->seed` (+ stable per-element keys), recomputed per frame.
- Must fill the entire buffer every frame (the device triple-buffers; the
  "previous frame" isn't well-defined).
- Must satisfy t=0 / t=1 contract (see plan.md) for every rotation/bg/seed.
- Shared helpers (bg fill, logo-pixel iteration, easing) go in the shared
  module so the existing pixel-blit semantics stay the single source of
  truth.
- Precomputed tables: prefer `static const` (flash/rodata). Runtime scratch
  policy if ever needed: manager-provided buffer in ctx, capped (~256 KB,
  PSRAM); harness enforces the same cap. Avoid until actually required.

### Time model

- Fixed sequence: `blank-delay` (250 ms, hardcoded) → `intro-animation`
  (NVS-configurable, default 3000 ms) → `hold` (1000 ms, hardcoded).
- Manager computes
  `t = clamp((now - start - BLANK_DELAY_MS) / intro_anim_ms, 0, 1)`.
  During `blank-delay` it draws plain background without calling the
  animation; during `hold` it draws the canonical end state itself
  (bg fill + full-opacity centered blit — identical to every animation's
  t=1 frame, so the handoff is invisible).
- Frame pacing: per-animation, from the registry's `frame_budget_ms`.
  Defaults to 33 ms (30 FPS); animations may opt into a different budget
  if they need it AND fit the device budget.
- `smoothstep-fade` mapping: `alpha = smoothstep(t)`, full window. Together
  with the manager's bookends this reproduces today's 250 + 2000 + 1000 ms
  exactly when the NVS duration is 2000 ms (the reference for Phase 2 parity
  testing).

## Configuration (NVS, web UI)

Two new NVS keys, both exposed in the **Display** tab of the Settings page:

| Key | Type | Range / values | Default | Notes |
|-----|------|----------------|---------|-------|
| `intro_anim_ms` | uint32 | 1000..7500 | 3000 | Duration of the animation window (ms). Clamped on read; out-of-range writes rejected by the API. |
| `intro_anim_force` | string | empty / `"random"` / a registered animation name | empty | When set to a valid name, that animation plays on every boot; otherwise random. Invalid names log a warning and fall back to random. |

Web UI:
- Duration: numeric input (or slider + numeric), step 100 ms.
- Force-animation: dropdown populated from the registry — "Random" plus one
  entry per animation. Doubles as the QA tool during Phase 3 (use the
  dropdown to verify each candidate on device).

## Host harness design

Three modes, one binary:

1. **Viewer** (default): Win32 + GDI (`StretchDIBits`) — zero external deps,
   single 720×720 window. Loop mirrors the device: wall-clock elapsed → t →
   render → present, sleeping to **the current animation's
   `frame_budget_ms`**. Keys: next/prev animation, `R` rotation, `B` cycle
   bg presets (black, white, saturated, mid-gray, the 0x808080 chroma-key
   gray), `Space` restart, `S` reroll seed, `J` toggle jittered pacing
   (random 40–120 ms — verifies designs read well when the P4 misses
   budget), `+/-` duration.
2. **Dump**: `--dump <dir> [--anim <name>] [--duration-ms 3000]` — renders
   frames at exact timestamps, writes `frame_%04d.bmp`, optionally shells
   out to ffmpeg (already installed) for an mp4. Output is deterministic
   given seed.
3. **Check**: `--check` — automated contract suite, run over **every**
   registered animation:
   - **t=0 exactness**: frame equals flat bg fill (memcmp).
   - **t=1 exactness**: frame equals reference end state (bg fill +
     `p3a_logo_blit_pixelwise_bgr888(255, scale=3, rotation)` centered).
   - **Determinism**: render the full sequence twice with the same seed →
     bit-equal; render frames out of order → same results.
   - **No out-of-bounds writes**: buffer allocated with canary bands
     verified intact after every frame.
   - **Matrix**: rotations {0,90,180,270} × bg colors {black, white,
     saturated, 0x808080, random} × durations {1000, 3000, 7500 ms} ×
     a few seeds. t=0/t=1 across the whole matrix; heavier checks on a
     subset.
   - Non-zero exit on any failure; concise per-animation pass/fail table.

The 0x808080 background case is a dedicated test: the logo's chroma key is
the same gray, so end-state references must come from the real blitter
(they do, by construction).

## Performance budget (ESP32-P4)

- Each animation declares its own per-frame budget via `frame_budget_ms`.
  Default **33 ms** (30 FPS, since 2026-06-12; was 40 ms/25 FPS); Phase 5
  profiling validates that the actual cost fits.
- Known-fits baseline: full-buffer clear + full pixelwise alpha blit
  (today's worst frame). New animations of similar shape — one or two
  sequential passes with integer per-pixel math — are safe at 33 ms.
- Guidance: integer math or LUTs per pixel (P4 has single-precision HW FPU:
  per-frame float scalars are fine, per-pixel float is suspect, per-pixel
  trig/sqrt is not OK — precompute); write rows sequentially (PSRAM rewards
  row-major streaming, punishes scattered writes).
- Animations that blow their budget degrade gracefully (time-driven →
  frames drop, duration holds) but should be fixed.
