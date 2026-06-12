# Intro-Animations Project

Replace the single hardcoded boot animation with a pool of **12 intro animations**,
one picked at random on every boot. Each is a different way of "fading in" the
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

**Phase: 3 IN PROGRESS — batch 3 (8 animations) ready for host review.**

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Host toolchain install (one-time, user action) | done — WinLibs MinGW-w64 / gcc 16.1.0 |
| 1 | Host harness: viewer, frame dump, automated checks | done — `host/intro-anim-lab/`; `--check` green |
| 2 | Architecture refactor + port `smoothstep-fade` | done — device-confirmed 2026-06-12 |
| 3 | Develop new animations (overshoot the final 12; cull to best 11) | 16 registered; batch 1+2 host-OK; batch 3 host-OK (pixel-rain, venetian, glitch-settle, typewriter, neon-trace, spiral-reveal, sparkle-fade, rotate-step); pixel-zoom rejected |
| 4 | Random selection + duration setting + force-override (web UI) | not started |
| 5 | Final device QA: profiling, rotations, bg colors | not started |

Animation roster: 16 implemented (`smoothstep-fade` device-OK; the other 15
host-OK awaiting device verification — overshooting on purpose so we can
cull to 12 after device QA). See [catalog.md](catalog.md) for the live
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
   on the ESP32-P4 by Fab) → `production-ready` (selected for the final 12,
   polished, profiled, signed off).
4. Nothing is implemented until Fab approves. The plan is finalized; concepts
   are picked batch by batch in Phase 3.

## Key facts (verified against code 2026-06-11)

- Current boot animation lives in `components/p3a_core/p3a_boot_logo.c` (+ `.h`),
  driven by `p3a_render_frame()` in `components/p3a_core/p3a_render.c:144`.
- Today's hardcoded timings: 250 ms blank delay + 2000 ms smoothstep fade +
  1000 ms hold = 3250 ms total; 40 ms/frame target (25 FPS).
- After this project, default total boot will be **4250 ms** (250 + 3000 +
  1000), with the middle phase user-configurable.
- Logo: 46×54 BGR888 with gray (0x808080) chroma key, in
  `components/p3a_core/p3a_logo.c`; blitted at **3× scale** (138×162 px),
  centered on 720×720, honoring rotation (0/90/180/270) and user-configured
  background color from `config_store`.
