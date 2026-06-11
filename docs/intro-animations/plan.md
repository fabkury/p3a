# Intro-Animations — Plan

Goal: 12 visually distinct intro animations, one picked at random per boot, all
sharing one architecture, all developed and regression-checked on the host before
device verification. Status tracking lives in [README.md](README.md) (phases) and
[catalog.md](catalog.md) (per-animation).

## Boot sequence (fixed structure)

```
blank-delay (250 ms, hardcoded)  ->  intro-animation (2000 ms, parameterized)  ->  hold (1000 ms, hardcoded)
```

- **`blank-delay`**: flat user-configured background color, no logo. Manager-owned.
- **`intro-animation`**: the per-boot randomly picked animation; the only part the
  12 modules implement; the only duration that is parameterized (`t` spans exactly
  this window).
- **`hold`**: static end state — logo at full opacity centered. Manager-owned.

Total boot wall time today: 250 + 2000 + 1000 = 3250 ms, identical to current firmware.

## Contract (what every animation must satisfy)

1. **First frame (t=0):** pixel-identical to a flat fill of the user-configured
   background color (i.e., identical to what `blank-delay` shows). No logo content
   whatsoever.
2. **Last frame (t=1):** pixel-identical to the canonical end state — background
   fill + `p3a_logo_blit_pixelwise_bgr888(alpha=255, scale=3, rotation)` centered.
   This is exactly what `hold` displays, so the animation→hold handoff is seamless
   for every animation.
3. **Pure function of time:** each frame is computed from normalized time
   `t ∈ [0,1]` plus a per-boot seed — no internal state across frames, no wall-clock
   reads inside the animation. (This is what makes duration changes, host/device
   parity, frame-skip tolerance, and determinism checks all work for free.)
4. **Honors runtime config:** background color (any RGB) and rotation (0/90/180/270).
5. **Real-time rendered** on the P4 within the per-frame budget (see
   [architecture.md](architecture.md) → performance budget).
6. **Duration-agnostic:** animations only ever see `t`; the total duration is a
   single constant owned by the manager.

## Phases

### Phase 0 — Host toolchain (one-time, user action)

No host C compiler exists on the laptop today (checked 2026-06-11: no gcc/cl/clang;
Python 3.14 and ffmpeg are present).

- [ ] Install a MinGW-w64 gcc distribution, e.g. `winget install BrechtSanders.WinLibs.POSIX.UCRT`
      (or w64devkit, or VS Build Tools — any C compiler works; plan assumes gcc).
- [ ] Verify: `gcc --version` from PowerShell.

No CMake/ninja needed: the harness is a handful of `.c` files built by a small
`build.ps1` that invokes gcc directly.

### Phase 1 — Host harness and shims

Build `host/intro-anim-lab/` (kept in the repo permanently). Details in
[architecture.md](architecture.md). Deliverables:

- [ ] `build.ps1` — compiles harness + shared animation sources with gcc.
- [ ] **Live viewer** (Win32/GDI, zero external deps): 720×720 window, plays
      animations with the same wall-clock-driven loop shape as the device,
      **capped at 25 FPS** so the preview is honest. Keyboard: switch animation,
      cycle rotation, cycle background colors, restart, reroll seed, toggle
      jittered-pacing stress mode.
- [ ] **Frame-dump mode**: deterministic render at fixed timestamps → BMP frames
      → optional ffmpeg assembly to mp4 (shareable clips; possible outreach asset).
- [ ] **Check mode**: automated contract suite (see architecture.md → automated
      checks): t=0/t=1 pixel-exactness, determinism, out-of-bounds canaries,
      across rotations × bg colors × durations × seeds. Non-zero exit on failure.
- [ ] Harness README with usage (so future sessions don't rediscover flags).

Acceptance: harness builds and runs on the laptop; check mode passes against a
trivial placeholder animation.

### Phase 2 — Architecture refactor + port `smoothstep-fade`

- [ ] Create `components/p3a_core/intro_anims/` with `intro_anim.h` (interface +
      context struct + registry) — **pure C, zero ESP-IDF includes** (the host
      build is the proof).
- [ ] Port the current animation as the first module, named **`smoothstep-fade`**:
      simply alpha = smoothstep(t) across the whole window. With the duration
      parameter at 2000 ms, plus the manager's blank-delay and hold, this
      reproduces today's 250 + 2000 + 1000 ms sequence exactly.
- [ ] Refactor `p3a_boot_logo.c` into the **intro manager**: keeps its public API
      (`p3a_boot_logo_init/is_active/render`) so `p3a_render.c` barely changes;
      internally owns the blank-delay and hold periods, the clock, normalized-t
      computation, the duration constant, seed generation, and (Phase 4) the
      random pick. During hold it draws the canonical end state itself (same as
      every animation's t=1 frame).
- [ ] Host: `smoothstep-fade` passes check mode + visually matches the old behavior.
- [ ] Device (Fab builds/flashes): boot looks identical to today.

Acceptance: device boot is visually indistinguishable from pre-refactor; check
suite green.

### Phase 3 — Develop the 11 new animations

Work in batches of ~3–4 animations to keep device-test round-trips efficient:

1. Pick concepts from [catalog.md](catalog.md) candidates (Fab approves each batch).
2. Implement on host; iterate in the live viewer (this is the fast loop —
   host rebuild is ~1 s vs minutes for firmware).
3. Pass check mode; review frame dumps at 25 FPS and under jittered pacing.
4. Fab builds firmware and verifies the batch on the ESP32-P4 (look + frame-time
   log; see Phase 5 profiling notes).
5. Debug/polish until `production-ready`; update catalog statuses.

- [ ] Batch 1 (3–4 animations): approved → host-OK → device-OK → production-ready
- [ ] Batch 2: same
- [ ] Batch 3: same (reaching 12 total)

Acceptance: 12 animations at `production-ready` in catalog.md.

### Phase 4 — Random selection + duration parameter

- [ ] Manager picks uniformly at random (`esp_random()`) at first render; **logs the
      chosen animation name at INFO** (operator-facing, same philosophy as the
      scheduler pick logs).
- [ ] Single duration constant `P3A_INTRO_ANIM_MS` (initially **2000 ms** = today's
      fade phase). `blank-delay` (250 ms) and `hold` (1000 ms) stay separate and
      hardcoded; total wall time unchanged at 3250 ms.
- [ ] Dev/QA override to force a specific animation (mechanism: open decision below).

Acceptance: ~uniform spread of animations over repeated boots; forced-pick override
works for QA.

### Phase 5 — Final device QA

- [ ] One profiling firmware pass: log per-frame render time (min/avg/max) for all
      12 animations on the P4; all within the 40 ms budget.
- [ ] Spot-check all 12 across rotations and a few background colors (incl. black,
      white, and a saturated color) on device.
- [ ] Docs sweep: update this folder's statuses, `docs/infrastructure/components.md`
      if it mentions the boot logo, and anything else referencing the old single
      animation.
- [ ] Harness documented and left in repo for future animation development.

## Decisions log

| Date | Decision |
|------|----------|
| 2026-06-11 | The 250 ms blank delay stays a **separate, hardcoded** phase in the manager, outside normalized t. |
| 2026-06-11 | The 1000 ms **`hold`** (static logo after the animation) is likewise a separate, hardcoded, manager-owned period — bookending the animation symmetrically with **`blank-delay`**. Sequence: blank-delay → intro-animation → hold. The parameterized duration covers only the intro-animation window: **2000 ms today**. Total boot wall time unchanged (3250 ms). |
| 2026-06-11 | Logo end-state scale confirmed as **3×** (not 2× as earlier recalled); 46×54 logo → 138×162 px centered. |
| 2026-06-11 | Code sharing strategy: animation modules are **pure C with zero ESP-IDF includes**, compiled verbatim by both firmware and host harness. No emulation layer; the manager (device) and harness (host) each own clock + config. |
| 2026-06-11 | Host harness lives at `host/intro-anim-lab/` and is **permanent** repo infrastructure. |
| 2026-06-11 | Host viewer is capped at 25 FPS to match the device frame cadence (no misleadingly smooth previews). |
| 2026-06-11 | Animations are pure functions of (t, seed); randomness comes from a per-boot seed in the context, never from wall clock or global RNG inside the module. |

## Open decisions (decide later, none block Phases 0–2)

- **Duration user-configurable?** Compile-time constant for now; NVS-backed
  setting (via `config_store` + web UI) is a one-line switch later thanks to the
  pure-t design. Fab undecided.
- **Force-override mechanism for QA:** compile-time `#define` (zero risk) vs NVS
  key vs debug HTTP endpoint. Leaning: compile-time define for Phase 3 testing;
  revisit if tedious.
- **Per-animation frame interval:** fixed 40 ms for all, or allow fast-motion
  animations to request 33 ms? Decide after the Phase 5 profiling pass shows
  headroom (or not).
- **Repeat-avoidance:** plain uniform random, or avoid repeating the previous
  boot's animation (would cost an NVS write per boot — flash-wear consideration)?
  Leaning: plain uniform; with 12 animations a repeat is 1/12 and harmless.
- **Final selection of the 11 new concepts** from catalog.md candidates.
