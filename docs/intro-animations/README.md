# Intro-Animations Project

Replace the single hardcoded boot animation with a pool of **22 intro animations**,
one picked at random on every boot. (The plan originally targeted 12 via a
cull; on 2026-08-10 Fab decided every implemented animation ships — no cull.)
Each is a different way of "fading in" the
p3a logo: the screen starts as flat user-configured background color and ends
with the logo statically centered — only what happens in between differs.

Boot sequence (fixed structure):

```
blank-delay (250 ms, hardcoded)  ->  intro-animation (NVS-configurable 1000..7500 ms, default 3000 ms)  ->  hold (1000 ms, hardcoded)
```

Development happens **on the host (Windows laptop) first**, using the exact same
animation source files the firmware compiles, then gets verified on the
ESP32-P4. The host harness stays in the repo permanently so more animations
can be added later.

## Status

**Phase: 5 — final device QA remaining.** All 22 animations and the Phase-4
controls are device-verified (Fab, 2026-08-10). Decision 2026-08-10: **the
cull to 12 is dropped — all 22 implemented animations ship.**

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Host toolchain install (one-time, user action) | done — WinLibs MinGW-w64 / gcc 16.1.0 |
| 1 | Host harness: viewer, frame dump, automated checks | done — `host/intro-anim-lab/`; `--check` green |
| 2 | Architecture refactor + port `smoothstep-fade` | done — device-confirmed 2026-06-12 |
| 3 | Develop new animations | done — 22 implemented, all in the firmware build, device-verified 2026-08-10; cull dropped (all ship); rejected: pixel-zoom, neon-trace, sparkle-fade, rotate-step, flood-fill, plasma-resolve, shutter-bands, pixel-shuffle, fire-burnup, bayer-reveal, rotozoom-settle, ripple-converge, twirl-unwind, slot-reels, diamond-wipe |
| 4 | Random selection + duration setting + force-override (web UI) | done — device-verified 2026-08-10 (esp_random pick, NVS `intro_anim_ms` 1000..7500 default 3000, NVS `intro_anim_force`, /api/intro-animations, Display tab dropdown+slider, per-frame timing log) |
| 5 | Final device QA: profiling, rotations, bg colors | pending — profiling pass, rotation + background-color spot-checks, duration min/default/max spot-checks (docs sweep done 2026-08-10) |

Animation roster: 22 implemented and device-verified; all 22 are registered
in the firmware and ship. See [catalog.md](catalog.md) for the live
roster and candidate concepts.

## Files

- [plan.md](plan.md) — phased plan with acceptance criteria.
- [architecture.md](architecture.md) — shared animation interface, host
  harness design, timing model, performance budget, automated checks, NVS keys.
- [catalog.md](catalog.md) — animation roster + candidate concepts.

## Session continuity (read me first when resuming)

This project spans many sessions/days/weeks. Conventions:

1. **Start of session:** read this README's status table, then the relevant
   phase in `plan.md`, then the roster in `catalog.md`.
2. **End of session:** update the status table above, tick checkboxes in
   `plan.md`, and update per-animation statuses in `catalog.md`.
3. **Animation lifecycle:** `idea` → `approved` → `host-dev` → `host-OK`
   (passes automated checks + looks right in viewer) → `device-OK` (verified
   on the ESP32-P4 by Fab) → `production-ready` (polished, profiled, signed
   off — since the cull was dropped 2026-08-10, every registered animation
   advances here once Phase 5 QA passes).
4. Nothing is implemented until Fab approves. The plan is finalized; concepts
   are picked batch by batch in Phase 3.

## Key facts (verified against code 2026-06-11)

- Current boot animation lives in `components/p3a_core/p3a_boot_logo.c` (+ `.h`),
  driven by `p3a_render_frame()` in `components/p3a_core/p3a_render.c:144`.
- Boot timings: 250 ms blank delay + NVS-configurable intro (default
  3000 ms) + 1000 ms hold; 33 ms/frame target (30 FPS).
- Default total boot is now **4250 ms** (250 + 3000 + 1000), with the middle
  phase user-configurable.
- Logo: 46×54 BGR888 with gray (0x808080) chroma key, in
  `components/p3a_core/p3a_logo.c`; blitted at **3× scale** (138×162 px),
  centered on 720×720, honoring rotation (0/90/180/270) and user-configured
  background color from `config_store`.
