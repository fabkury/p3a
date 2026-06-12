# Intro-Animations — Catalog

Live status of every animation that exists, was implemented, or is a
candidate for implementation. Status values (defined in
[README.md](README.md)): `idea` → `approved` → `host-dev` → `host-OK` →
`device-OK` → `production-ready`.

Final selection strategy: **build more candidates than the 12 final slots
need, then keep only the best 11** (plus `smoothstep-fade`) for
`production-ready`. Rejected candidates that reached `device-OK` stay in this
catalog but are not registered in firmware.

Update this file whenever an animation changes status.

## Implemented animations

| Name | Status | Notes |
|------|--------|-------|
| `smoothstep-fade` | device-OK (2026-06-12) | The legacy boot animation behavior, frozen: `alpha = smoothstep(t)` across the full window. Module: `ia_smoothstep_fade.c`. |
| `pixel-dissolve`  | host-OK (batch 1, 2026-06-12) | Per-source-pixel threshold dissolve; threshold derived from seed via `intro_anim_hash3`. Pixel-art preserved (one threshold per source pixel, not per dest). Module: `ia_pixel_dissolve.c`. |
| `iris-wipe`       | host-OK (batch 1, 2026-06-12) | Circular reveal from logo center to bbox corners; smoothstep-paced radius. Module: `ia_iris_wipe.c`. |
| `assemble`        | host-OK (batch 1, 2026-06-12) | Per-source-pixel fly-in from one of 8 cardinal/diagonal directions plus jitter; offset magnitude decays via cubic ease-out (fast start, slow settle). Module: `ia_assemble.c`. |

## Candidate pool

Working names are provisional. "Perf risk" = likelihood of needing
optimization to fit the per-frame budget on the P4 (see
[architecture.md](architecture.md)). All candidates must satisfy the
t=0/t=1 contract; concepts below describe only the middle.

Each batch of candidates is approved by Fab from this pool, implemented on
host, verified on device, then either advances toward selection or is set
aside.

| Candidate | Concept | Perf risk | Status |
|-----------|---------|-----------|--------|
| `pixel-dissolve` | Logo pixels appear in pseudo-random order via a per-pixel threshold map (ordered/blue-noise dither against t) | low | implemented (see above) |
| `scanline-reveal` | CRT-style bright scan bar sweeps down; logo revealed above the bar | low | idea |
| `pixel-rain` | Logo pixels fall from the top edge and land in place, bottom rows first | medium | idea |
| `assemble` | Logo pixels fly in from screen edges and converge on final positions | medium | implemented (see above) |
| `pixel-zoom` | Logo grows from 1 px at screen center to final size, nearest-neighbor steps | low | rejected 2026-06-12 (visual didn't read as a logo reveal — Fab) |
| `checker-tiles` | Logo appears tile-by-tile (e.g. 8×8 px tiles) in checkerboard / shuffled order, each tile popping in | low | idea |
| `iris-wipe` | Circular iris opens from center revealing the logo | low | implemented (see above) |
| `venetian` | Alternating horizontal strips slide in from left/right and lock into place | low | idea |
| `glitch-settle` | Logo appears as horizontally-sliced RGB-split glitch bands that jitter and settle into the clean logo | medium | idea |
| `typewriter` | Logo columns (or glyph-like chunks) appear left-to-right with a blinking cursor block | low | idea |
| `bounce-drop` | Logo drops from above, squash-and-stretch bounce, settles centered | medium | idea |
| `wave-settle` | Logo visible early but rows displaced by a horizontal sine wave whose amplitude damps to zero | medium | idea |
| `sparkle-fade` | Fade-in (like #1) plus seed-placed twinkling sparkle pixels that die out by t=1 | low | idea |
| `spiral-reveal` | Logo pixels revealed in spiral order from center outward | low | idea |
| `neon-trace` | Logo outline traces in (edge pixels light up progressively along the perimeter), then fills to solid | medium | idea |
| `plasma-resolve` | A colorful plasma field (LUT-based) over the bg resolves/condenses into the logo as t→1 | high | idea |
| `rotate-step` | Logo snaps through 90°-step rotations (pixel-art-safe) while scaling up into place | low | idea |

Selection guidance when picking batches: favor variety of *mechanism*
(dissolve vs motion vs wipe vs distortion vs zoom) over variety of theme,
so the final 12 feel genuinely different at a glance. Keep at most one
`high` perf-risk concept per batch so device round-trips aren't dominated
by optimization work.
