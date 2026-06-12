# Intro-Animations — Plan

Goal: 12 visually distinct intro animations, one picked at random per boot, all
sharing one architecture, all developed and regression-checked on the host
before device verification. Status tracking lives in [README.md](README.md)
(phases) and [catalog.md](catalog.md) (per-animation).

## Boot sequence (fixed structure)

```
blank-delay (250 ms, hardcoded)  ->  intro-animation (NVS, 1000..7500 ms, default 3000 ms)  ->  hold (1000 ms, hardcoded)
```

- **`blank-delay`**: flat user-configured background color, no logo.
  Manager-owned.
- **`intro-animation`**: the per-boot randomly picked animation; the only part
  the 12 modules implement; the only duration that is parameterized (`t`
  spans exactly this window).
- **`hold`**: static end state — logo at full opacity centered. Manager-owned.

## Contract (what every animation must satisfy)

1. **First frame (t=0):** pixel-identical to a flat fill of the user-configured
   background color (i.e. identical to what `blank-delay` shows). No logo
   content whatsoever.
2. **Last frame (t=1):** pixel-identical to the canonical end state —
   background fill + `p3a_logo_blit_pixelwise_bgr888(alpha=255, scale=3,
   rotation)` centered. This is exactly what `hold` displays, so the
   animation→hold handoff is seamless for every animation.
3. **Pure function of time:** each frame is computed from normalized time
   `t ∈ [0,1]` plus a per-boot seed — no internal state across frames, no
   wall-clock reads inside the animation.
4. **Honors runtime config:** background color (any RGB) and rotation
   (0/90/180/270).
5. **Real-time rendered** on the P4 within its declared per-frame budget (see
   [architecture.md](architecture.md) → performance budget).
6. **Duration-agnostic:** animations only ever see `t`; total duration is
   owned by the manager via the NVS-backed setting.

## Phases

### Phase 0 — Host toolchain (one-time, user action)

- [x] Install MinGW-w64 gcc, e.g. `winget install BrechtSanders.WinLibs.POSIX.UCRT`.
- [x] Verify: `gcc --version` from PowerShell.

Installed: WinLibs MinGW-w64 (UCRT/POSIX) gcc 16.1.0, at
`C:\Users\fab\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin\gcc.exe`.
The `build.ps1` falls back to that exact path if `gcc` isn't on the PATH yet
(new shell session needed).

No CMake/ninja used: the harness is a handful of `.c` files built by a small
`build.ps1` invoking gcc directly.

### Phase 1 — Host harness and shims

Built at `host/intro-anim-lab/`. Single `build.ps1` produces
`build/intro-anim-lab.exe`. Compiles `components/p3a_core/p3a_logo.c` and
`components/p3a_core/intro_anims/*.c` verbatim — host build doubles as the
purity proof for those files.

- [x] `build.ps1` — compiles harness + shared animation sources with gcc.
- [x] **Live viewer** (Win32/GDI, zero external deps): 720×720 window,
      wall-clock-driven loop, paced at each animation's declared frame
      budget. Keys: Space (replay), N/P (anim), R (rotation), B (bg
      preset), S (reroll seed), +/- (duration ±500 ms, clamped 1000–7500),
      Esc.
- [x] **Frame-dump mode**: deterministic render → `frame_%04d.bmp` per
      directory; ffmpeg-assembly is one shell line away (already on
      laptop). Sample run produced 76 frames at 25 FPS for a 3 s window.
- [x] **Check mode**: t=0 / t=1 pixel-exactness, determinism, out-of-bounds
      canaries, across {5 bg colors} × {4 rotations} × {3 seeds}. Non-zero
      exit on failure.
- [x] Harness README with usage (`host/intro-anim-lab/README.md`).

Note: jittered-pacing stress toggle was descoped — it would only matter for
animations the device can't keep up with, and there's nothing yet to stress.
Add later if Phase 5 profiling finds budget overruns.

Acceptance: harness builds and runs on the laptop; check mode passes
(`smoothstep-fade` exits 0).

### Phase 2 — Architecture refactor + port `smoothstep-fade`

- [x] Create `components/p3a_core/intro_anims/` with `intro_anim.h`
      (interface + context struct + registry declaration), plus
      `intro_anim_common.c`, `intro_anim_registry.c`, and
      `ia_smoothstep_fade.c`. **Pure C, zero ESP-IDF includes** — verified
      by the host build that compiles them unchanged.
- [x] Port the current animation as the first module, named
      **`smoothstep-fade`**: `alpha = smoothstep(t)` across the full window.
- [x] Refactor `p3a_boot_logo.c` into the **intro manager**: public API
      (`p3a_boot_logo_init/is_active/render`) preserved so `p3a_render.c`
      didn't change. Internally owns blank-delay and hold bookends, the
      clock, normalized-t, seed (from `esp_timer_get_time()` — Phase 4
      will switch to `esp_random()`). Duration is the legacy 2000 ms
      constant for parity (Phase 4 adds NVS read with default 3000).
      During hold the manager calls `anim->render(.., t=1.0f)` itself.
- [x] Host: `smoothstep-fade` passes check mode.
- [x] Device (Fab builds/flashes): boot looks identical to today (verified 2026-06-12).

Acceptance: device boot matches pre-refactor; check suite green. **DONE.**

### Phase 3 — Develop new animations (more than 11, cull to best 11)

Strategy: **build more than we need, keep only the best 11**. Concepts move
from the candidate pool in [catalog.md](catalog.md) into implementation in
batches; final selection happens after side-by-side comparison on device.

For each batch:

1. Fab approves the batch's concepts from the candidate pool.
2. Implement on host; iterate in the live viewer.
3. Pass check mode; review frame dumps and jittered-pacing behavior.
4. Fab builds firmware and reviews the batch on the ESP32-P4.
5. Polish to `device-OK`; mark candidate status in catalog.

After enough candidates have reached `device-OK`, Fab picks the **best 11**
(plus `smoothstep-fade` for 12 total) and they advance to `production-ready`.
Rejected candidates stay in the catalog as `device-OK` (could be revived
later) but are not registered in the firmware.

Acceptance: 12 animations at `production-ready` in catalog.md.

### Phase 4 — Random selection + duration setting + force-override

- [ ] Manager picks uniformly at random (`esp_random()`) at first render;
      logs the chosen animation name at INFO (operator-facing, like
      `ps_pick`).
- [ ] **NVS duration setting**: key `intro_anim_ms` (clamped 1000..7500,
      default 3000). Read once per boot.
- [ ] **NVS force-override setting**: key `intro_anim_force` (string;
      empty/"random" = random, else animation name). When set to a valid
      registered animation, that animation plays every boot. Invalid name →
      log warning, fall back to random.
- [ ] **Web UI controls** in the Display tab of Settings:
  - Duration slider/numeric input (ms), 1000..7500, step 100, default 3000.
  - Force-animation dropdown: "Random" + one entry per registered animation.
- [ ] HTTP API endpoints to read/write both settings (consistent with the
      existing config_store/web UI pattern).

Acceptance: ~uniform spread over repeated boots with default settings;
duration changes take effect on next boot; force-override plays the selected
animation deterministically.

### Phase 5 — Final device QA

- [ ] One profiling firmware pass: log per-frame render time (min/avg/max)
      for all 12 animations on the P4; each must fit its declared per-frame
      budget.
- [ ] Spot-check all 12 across rotations and a few background colors (incl.
      black, white, 0x808080 — the chroma-key gray — and a saturated color)
      on device.
- [ ] Spot-check the duration setting at min (1000 ms), default (3000 ms),
      and max (7500 ms).
- [ ] Docs sweep: `docs/intro-animations/` statuses, `docs/HOW-TO-USE.md` for
      the new Display setting, `docs/infrastructure/components.md` if it
      mentions the boot logo.
- [ ] Harness documented and left in repo for future animation development.
