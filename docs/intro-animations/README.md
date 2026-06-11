# Intro-Animations Project

Replace the single hardcoded boot animation with a pool of **12 intro animations**,
one picked at random on every boot. Each animation is a different way of "fading in"
the p3a logo: the screen starts as flat user-configured background color and ends
with the logo statically centered — only what happens in between differs.

The boot screen sequence is fixed:
**`blank-delay`** (250 ms, hardcoded, plain background) →
**`intro-animation`** (2000 ms, the parameterized window the 12 animations fill) →
**`hold`** (1000 ms, hardcoded, static centered logo).

Development happens **on the host (Windows laptop) first**, using the exact same
animation source files the firmware compiles, then gets verified on the ESP32-P4.
The host harness stays in the repo permanently so more animations can be added later.

## Status

**Phase: PLANNING — plan drafted 2026-06-11, awaiting Fab's review. No implementation yet.**

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Host toolchain install (one-time, user action) | not started |
| 1 | Host harness: viewer, frame dump, automated checks | not started |
| 2 | Architecture refactor + port `smoothstep-fade` | not started |
| 3 | Develop 11 new animations (host → device, in batches) | not started |
| 4 | Random selection wiring + duration parameter | not started |
| 5 | Final device QA: profiling, rotations, bg colors | not started |

Animation roster: 1 of 12 exists (`smoothstep-fade`, the current boot animation).
See [catalog.md](catalog.md) for the live roster and candidate concepts.

## Files

- [plan.md](plan.md) — the full phased plan with acceptance criteria, decisions made, and open decisions
- [architecture.md](architecture.md) — shared animation interface, host harness design, timing model, performance budget, automated checks
- [catalog.md](catalog.md) — animation roster with per-animation status, plus candidate concepts for the 11 new slots

## Session continuity (read me first when resuming)

This project spans many sessions/days/weeks. Conventions to keep it coherent:

1. **Start of session:** read this README's status table, then the relevant phase in
   `plan.md`, then the roster in `catalog.md`.
2. **End of session:** update the status table above, tick checkboxes in `plan.md`,
   and update per-animation statuses in `catalog.md`. Record any new decision in
   `plan.md`'s decisions log (with date).
3. **Statuses for animations** (used in catalog.md): `idea` → `approved` →
   `host-dev` → `host-OK` (passes automated checks + looks right in viewer) →
   `device-OK` (verified on the ESP32-P4 by Fab) → `production-ready` (polished,
   profiled, signed off).
4. Nothing is implemented until Fab approves the plan (and, for each new animation,
   the concept).

## Key facts (verified against code 2026-06-11)

- Current boot animation lives in `components/p3a_core/p3a_boot_logo.c` (+ `.h`),
  driven by `p3a_render_frame()` in `components/p3a_core/p3a_render.c:144`.
- Timing today: 250 ms blank delay + 2000 ms smoothstep fade + 1000 ms hold = 3250 ms
  total; 40 ms/frame target (25 FPS). These map directly onto the new model:
  blank-delay (250, hardcoded) / intro-animation (2000, parameterized) /
  hold (1000, hardcoded) — total wall time unchanged.
- Logo: 46×54 BGR888 with gray (0x808080) chroma key, in `components/p3a_core/p3a_logo.c`;
  blitted at **3× scale** (138×162 px), centered on 720×720, honoring rotation
  (0/90/180/270) and user-configured background color from `config_store`.
