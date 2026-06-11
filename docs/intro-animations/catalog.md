# Intro-Animations — Catalog

Live roster of the 12 animation slots plus the candidate pool for the unfilled
ones. Status values (defined in [README.md](README.md)): `idea` → `approved` →
`host-dev` → `host-OK` → `device-OK` → `production-ready`.

Update this file whenever an animation changes status.

## Roster (12 slots)

| # | Name | Status | Notes |
|---|------|--------|-------|
| 1 | `smoothstep-fade` | approved (port in Phase 2) | The current boot animation, behavior frozen: alpha = smoothstep(t) across the full window. (The static hold afterwards is manager-owned, not part of this module.) |
| 2 | — | open | |
| 3 | — | open | |
| 4 | — | open | |
| 5 | — | open | |
| 6 | — | open | |
| 7 | — | open | |
| 8 | — | open | |
| 9 | — | open | |
| 10 | — | open | |
| 11 | — | open | |
| 12 | — | open | |

## Candidate pool (pick 11 with Fab, batch by batch)

Working names are provisional. "Perf risk" = likelihood of needing optimization to
fit the 40 ms/frame budget on the P4 (see architecture.md). All candidates must
satisfy the t=0/t=1 contract; concepts below describe only the middle.

| Candidate | Concept | Perf risk | Notes |
|-----------|---------|-----------|-------|
| `pixel-dissolve` | Logo pixels appear in pseudo-random order via a per-pixel threshold map (ordered/blue-noise dither against t) | low | Classic, cheap (one LUT compare per logo pixel). Very pixel-art. |
| `scanline-reveal` | CRT-style bright scan bar sweeps down; logo revealed above the bar | low | Bar glow = small additive band; rest is row gating. |
| `pixel-rain` | Logo pixels fall from the top edge and land in place, bottom rows first | medium | Per-column offsets; keep writes row-ordered (composite per row, not per particle). |
| `assemble` | Logo pixels fly in from screen edges and converge on final positions | medium | Position = lerp(start(seed), final, ease(t)); needs row-ordered compositing pass. |
| `pixel-zoom` | Logo grows from 1 px at screen center to final size, nearest-neighbor steps (1×, 2×, 3× … or continuous integer-ish zoom) | low | Reuses scale-blit logic; extremely on-brand for a pixel-art device. |
| `checker-tiles` | Logo appears tile-by-tile (e.g. 8×8 px tiles) in checkerboard / shuffled order, each tile popping in | low | Tile order from seed; per-tile alpha pop. |
| `iris-wipe` | Circular iris opens from center revealing the logo | low | Radius compare per logo pixel via precomputed r² table. |
| `venetian` | Alternating horizontal strips slide in from left/right and lock into place | low | Per-strip x-offset; row-sequential by nature. |
| `glitch-settle` | Logo appears as horizontally-sliced RGB-split glitch bands that jitter and settle into the clean logo | medium | Jitter amplitude decays with t; seed-driven band offsets; must end exactly clean. |
| `typewriter` | Logo columns (or glyph-like chunks) appear left-to-right with a blinking cursor block | low | Column gating + cursor rectangle. |
| `bounce-drop` | Logo drops from above, squash-and-stretch bounce, settles centered | medium | Vertical offset + integer squash scaling; damped bounce easing ending at rest. |
| `wave-settle` | Logo visible early but rows displaced by a horizontal sine wave whose amplitude damps to zero | medium | Per-row offset from small LUT; amplitude × decay(t). |
| `sparkle-fade` | Fade-in (like #1) plus seed-placed twinkling sparkle pixels that die out by t=1 | low | Fade base + a few dozen sparkle points; cheap. |
| `spiral-reveal` | Logo pixels revealed in spiral order from center outward | low | Precomputed per-pixel spiral rank vs t threshold (same machinery as `pixel-dissolve` with a different map). |
| `neon-trace` | Logo outline traces in (edge pixels light up progressively along the perimeter), then fills to solid | medium | Needs an edge-order table (can be precomputed at build time); fill phase = alpha ramp. |
| `plasma-resolve` | A colorful plasma field (LUT-based) over the bg resolves/condenses into the logo as t→1 | high | Full-screen per-pixel effect; needs LUT discipline; visually strongest if it fits budget. |
| `rotate-step` | Logo snaps through 90°-step rotations (pixel-art-safe) while scaling up into place | low | Only 90° multiples (smooth rotation looks bad on pixel art); reuses rotation blit paths. |

Selection guidance when picking batches: favor variety of *mechanism* (dissolve vs
motion vs wipe vs distortion vs zoom) over variety of theme, so the 12 feel
genuinely different at a glance. Keep at most one `high` perf-risk concept per
batch so device round-trips aren't dominated by optimization work.
