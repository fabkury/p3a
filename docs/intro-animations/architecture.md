# Intro-Animations — Architecture

How 12 animations share one codebase across firmware and a Windows host harness.
Phase tracking is in [plan.md](plan.md); this file is the technical design.

## Current code (baseline, verified 2026-06-11)

- `components/p3a_core/p3a_boot_logo.c` — the whole current animation: reads
  `esp_timer_get_time()`, computes phase (250 ms delay / 2000 ms smoothstep fade /
  1000 ms hold), clears the buffer to bg color, blits the logo. Returns 40 ms as
  the next-frame delay; returns -1 when expired.
- `components/p3a_core/p3a_logo.c` — 46×54 BGR888 logo with 0x808080 chroma key +
  `p3a_logo_blit_pixelwise_bgr888()` (alpha, scale, rotation, clipping). **Already
  pure C** (only stdint) — compiles on host unchanged.
- `components/p3a_core/p3a_render.c:144` — calls the boot logo with highest
  priority; the returned delay paces the render task (`main/animation_player.c:92`).
- Buffer: BGR888, 720×720, stride passed in (~1.48 MB/frame), framebuffer in PSRAM.

Key observation: the animation code is already decoupled from the LCD/DMA/VSYNC
machinery — it just writes into a (buffer, w, h, stride). That is what makes host
development cheap.

## Target layout

```
components/p3a_core/
  p3a_boot_logo.c            # becomes the intro MANAGER (device-only):
                             #   clock, blank-delay + hold bookends, normalized t,
                             #   duration constant, seed from esp_random(),
                             #   random pick, INFO pick log.
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
  viewer_win32.c             # GDI window (StretchDIBits), 25 FPS-capped playback
  dump.c                     # BMP frame dump + optional ffmpeg assembly
  checks.c                   # automated contract suite
```

Firmware compiles `intro_anims/*.c` via `p3a_core`'s CMakeLists. The host build
compiles the same files plus `p3a_logo.c` with `-I` pointing at the component's
include dirs. **The host build doubles as the purity proof**: if someone adds an
`esp_log.h` include to an animation module, the host build breaks immediately.
No shim headers are needed at all under this split — shims were only necessary if
we compiled the manager on host, and we deliberately don't (the harness replaces it).

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
    const char *name;            // kebab-case, e.g. "smoothstep-fade"
    void (*render)(uint8_t *buffer, const intro_anim_ctx_t *ctx, float t);
} intro_anim_t;

extern const intro_anim_t intro_anim_registry[];
extern const int intro_anim_count;
```

Rules for `render()` implementations:

- Output for a given (ctx, t) must be **bit-exact deterministic** — no statics that
  persist across frames, no wall clock, no global RNG. Randomness = small inline
  PRNG (e.g. splitmix32/xorshift) seeded from `ctx->seed` (+ stable per-element
  keys like pixel index), recomputed per frame. This makes frame-skips harmless
  and lets the harness verify determinism mechanically.
- Must fill the entire buffer every frame (no reliance on previous frame contents —
  the device triple-buffers, so "previous frame" isn't even well-defined).
- Must satisfy t=0 / t=1 contract (see plan.md) for every rotation/bg/seed.
- Shared helpers (bg fill, logo-pixel iteration with chroma-key/scale/rotation
  mapping, easing functions) go in the shared module so the existing
  pixel-blit semantics stay the single source of truth. The dedup-audit's "image
  blitter" tier may eventually intersect here; don't block on it.
- If an animation needs precomputed tables, prefer `static const` (flash/rodata).
  Runtime scratch policy if ever needed: manager-provided buffer in ctx, capped
  (~256 KB, PSRAM); harness enforces the same cap. Avoid until an animation
  actually requires it.

### Time model

- Fixed sequence (see plan.md): `blank-delay` (250 ms, hardcoded) →
  `intro-animation` (`P3A_INTRO_ANIM_MS` = **2000 ms** today, the only
  parameterized duration) → `hold` (1000 ms, hardcoded).
- Manager computes `t = clamp((now - start - BLANK_DELAY_MS) / P3A_INTRO_ANIM_MS, 0, 1)`.
  During `blank-delay` it draws plain background without calling the animation;
  during `hold` it draws the canonical end state itself (bg fill + full-opacity
  centered blit — identical to every animation's t=1 frame, so the handoff is
  invisible). The animation module is only called within its own window.
- `smoothstep-fade` mapping: alpha = smoothstep(t), full window. Together with the
  manager's bookends this reproduces today's 250 + 2000 + 1000 ms exactly.
- Frame pacing stays manager-owned: 40 ms (25 FPS). Animations never pace.
- Duration changes (compile-time now, maybe NVS later) require zero animation edits.

## Host harness design

Three modes, one binary:

1. **Viewer** (default): Win32 + GDI (`StretchDIBits`) — zero external
   dependencies, single window 720×720. Loop mirrors the device: wall-clock
   elapsed → t → render → present, sleeping to a 40 ms cadence so what you see is
   what the device shows. Keys: next/prev animation, `R` rotation, `B` cycle bg
   presets (black, white, saturated, mid-gray), `Space` restart, `S` reroll seed,
   `J` toggle jitter mode (random 40–120 ms frame intervals — verifies designs
   read well when the P4 misses budget), `+/-` duration.
2. **Dump**: `--dump <dir> [--anim <name>] [--fps 25] [--duration-ms 2000]` —
   renders frames at exact timestamps, writes `frame_%04d.bmp` (trivial encoder),
   then optionally shells out to ffmpeg (already installed) for an mp4. Output is
   deterministic given seed → diffable across code changes.
3. **Check**: `--check` — the automated contract suite, run over **every**
   registered animation:
   - **t=0 exactness**: frame equals flat bg fill (memcmp).
   - **t=1 exactness**: frame equals reference end state (bg fill +
     `p3a_logo_blit_pixelwise_bgr888(255, scale=3, rotation)` centered).
   - **Determinism**: render the full sequence twice with the same seed → bit-equal;
     render frames out of order → same results (catches hidden state).
   - **No out-of-bounds writes**: buffer allocated with canary bands (extra stride
     padding + guard rows filled with a sentinel) → verified intact after every frame.
   - **Matrix**: rotations {0,90,180,270} × bg colors {black, white, saturated,
     0x808080 (chroma-key color — worth a dedicated case), random} × durations
     {1000, 2000, 10000 ms} × a few seeds. t=0/t=1 checks run across the whole
     matrix; heavier checks on a subset.
   - Non-zero exit code on any failure; concise per-animation pass/fail table.

Note the 0x808080 background case: the logo's chroma key is the same gray. The
existing blitter treats those logo pixels as transparent regardless of background,
so end-state references must come from the real blitter (they do, by construction)
rather than naive expectations.

## Performance budget (ESP32-P4)

- Budget: **40 ms/frame** at 720×720×3 BGR888 (~1.48 MB) in PSRAM.
- Known-fits baseline: full-buffer clear + full pixelwise alpha blit (today's
  worst frame). New animations of similar shape — one or two sequential passes
  with integer per-pixel math — are safe.
- Guidance: integer math or LUTs per pixel (P4 has single-precision HW FPU:
  per-frame float scalars are fine, per-pixel float is suspect, per-pixel
  trig/sqrt is not OK — precompute); write rows sequentially (PSRAM rewards
  row-major streaming, punishes scattered writes — particle effects should
  composite into row-ordered passes, not plot randomly).
- The host cannot measure device cost. Verification is empirical: Phase 5
  profiling build logs per-frame min/avg/max render time per animation.
- Animations that blow the budget degrade gracefully (time-driven → frames drop,
  duration holds) but should be fixed or simplified anyway.

## Device-side selection (Phase 4)

- Seed + pick from `esp_random()` at first render (entropy fine by then).
- Pick log at INFO with the animation name — operator-facing, like the
  scheduler's `ps_pick` lines; never demote to debug.
- QA override mechanism: open decision (plan.md); leaning compile-time define.
