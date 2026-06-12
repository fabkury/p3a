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
| `pixel-dissolve`  | host-OK (batch 1, 2026-06-12) | Hash-priority threshold dissolve: K = smoothstep(t)·N opaque source pixels visible, chosen by lowest seeded hash. Pixel-art preserved (one threshold per source pixel). Module: `ia_pixel_dissolve.c`. |
| `iris-wipe`       | host-OK (batch 1, 2026-06-12) | Circular reveal from bbox center shifted -10 src px upward; smoothstep-paced radius. Module: `ia_iris_wipe.c`. |
| `assemble`        | host-OK (batch 1, 2026-06-12) | Per-source-pixel fly-in over a 256-entry sin/cos LUT (continuous angle) with per-pixel magnitude variance in [0.6, 1.4]; cubic ease-out timing. Module: `ia_assemble.c`. |
| `scanline-reveal` | host-OK (batch 2, 2026-06-12) | CRT-style bright bar sweeps top→bottom across the bbox; logo opaque above the bar, bg below; bar contrast color picked against bg luminance. Module: `ia_scanline_reveal.c`. |
| `bounce-drop`     | host-OK (batch 2, 2026-06-12) | Whole-image gravity drop with damped bounces: falling arc accelerates linearly under gravity (y = H·(1−u²) → speed grows into impact), abrupt velocity reversal at landing, 2 damped bounces with restitution e=0.42, then holds. Module: `ia_bounce_drop.c`. |
| `wave-settle`     | host-OK (batch 2, 2026-06-12) | Horizontal sine displacement per source-row, amplitude damps to 0 by t=1 with phase drift; logo alpha fades in via smoothstep. Module: `ia_wave_settle.c`. |
| `checker-tiles`   | host-OK (batch 2, 2026-06-12) | 6×6 source-pixel tiles pop in via hash-priority threshold (smoothstep-paced) — chunky checkerboard-y reveal. Module: `ia_checker_tiles.c`. |
| `pixel-rain`      | host-OK (batch 3, 2026-06-12) | Per-source-pixel drop with three independent variability axes from one seeded hash: start delay ∈ [0, 0.55), drop distance ∈ [0.55, 1.7]×screen, ease-power ∈ {4,5,6,7}. Heterogenous rain rather than dense cloud. Module: `ia_pixel_rain.c`. |
| `venetian`        | host-OK (batch 3, 2026-06-12) | 6-src-px horizontal strips slide in alternately from left/right; smoothstep-paced offset to 0. Module: `ia_venetian.c`. |
| `glitch-settle`   | host-OK (batch 3, 2026-06-12) | 4-src-px-tall slabs jitter horizontally with per-channel R/G/B chromatic aberration; quantized into 12 shake states; (1−t)² decay. Module: `ia_glitch_settle.c`. |
| `typewriter`      | host-OK (batch 3, 2026-06-12) | Source-columns reveal left-to-right at constant speed; 6-cycle blinking contrast cursor at the head. Module: `ia_typewriter.c`. |
| `spiral-reveal`   | host-OK (batch 3, 2026-06-12) | Rotating cone sweeps 1.5 turns clockwise from bbox center; aperture and radial threshold both expand to full coverage by t=1. Module: `ia_spiral_reveal.c`. |
| `mosaic-shrink`   | host-OK (batch 4, 2026-06-12) | Block-quantized render at decreasing block size (12→1 src px); per-block top-left sample blended toward bg by smoothstep alpha. Module: `ia_mosaic_shrink.c`. |
| `diamond-wipe`    | host-OK (batch 4, 2026-06-12) | L1 (Manhattan) distance reveal from bbox center shifted -10 src px upward; rotated-square iris grows smoothstep-paced. Distinct metric from iris-wipe. Module: `ia_diamond_wipe.c`. |
| `color-emerge`    | host-OK (batch 4, 2026-06-12) | Two-phase color-space animation: silhouette in contrast color fades in alpha-wise over t∈[0, 0.25), then per-channel value lerps from silhouette toward source color over t∈[0.25, 1]. Module: `ia_color_emerge.c`. |
| `starburst`       | host-OK (batch 4, 2026-06-12) | Per-pixel fly-in from far outside along home-radial direction with back-out (overshoot) easing (c2=2.6, dist=1.7×width) — pixels punch ~22% past home before snapping back. Module: `ia_starburst.c`. |
| `plasma-dissolve` | host-OK (batch 5, 2026-06-12) | Smooth plasma threshold field (3-sin sum, seed-driven phases) at source-pixel scale; reveal iff field ≤ smoothstep(t). Same dissolve geometry as pixel-dissolve, but correlated noise → organic blobs instead of speckle. Module: `ia_plasma_dissolve.c`. |
| `voronoi-shatter` | host-OK (batch 5, 2026-06-12) | 24 seeded Voronoi sites; each cell rigid-translates from its outward radial direction with cubic ease-out remaining-distance. Cell-id LUT computed per call. Module: `ia_voronoi_shatter.c`. |
| `hue-cycle-lock`  | host-OK (batch 5, 2026-06-12) | Per-pixel HSV hue rotation; offset = (1−smoothstep(t))·2 full turns; saturation/value preserved. Logo fades in (smoothstep alpha) over the first 30% of the window while hue-cycling, then continues cycling at full opacity until locking to true colors at t=1. Pure color-space animation distinct from color-emerge (RGB lerp). Module: `ia_hue_cycle_lock.c`. |

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
| `scanline-reveal` | CRT-style bright scan bar sweeps down; logo revealed above the bar | low | implemented (see above) |
| `pixel-rain` | Logo pixels fall from the top edge and land in place, bottom rows first | medium | implemented (see above) |
| `assemble` | Logo pixels fly in from screen edges and converge on final positions | medium | implemented (see above) |
| `pixel-zoom` | Logo grows from 1 px at screen center to final size, nearest-neighbor steps | low | rejected 2026-06-12 (visual didn't read as a logo reveal — Fab) |
| `checker-tiles` | Logo appears tile-by-tile (e.g. 8×8 px tiles) in checkerboard / shuffled order, each tile popping in | low | implemented (see above) |
| `iris-wipe` | Circular iris opens from center revealing the logo | low | implemented (see above) |
| `venetian` | Alternating horizontal strips slide in from left/right and lock into place | low | implemented (see above) |
| `glitch-settle` | Logo appears as horizontally-sliced RGB-split glitch bands that jitter and settle into the clean logo | medium | implemented (see above) |
| `typewriter` | Logo columns (or glyph-like chunks) appear left-to-right with a blinking cursor block | low | implemented (see above) |
| `bounce-drop` | Logo drops from above, squash-and-stretch bounce, settles centered | medium | implemented (see above) |
| `wave-settle` | Logo visible early but rows displaced by a horizontal sine wave whose amplitude damps to zero | medium | implemented (see above) |
| `sparkle-fade` | Fade-in (like #1) plus seed-placed twinkling sparkle pixels that die out by t=1 | low | rejected 2026-06-12 (Fab) |
| `spiral-reveal` | Logo pixels revealed in spiral order from center outward | low | implemented (see above) |
| `neon-trace` | Logo outline traces in (edge pixels light up progressively along the perimeter), then fills to solid | medium | rejected 2026-06-12 (Fab) |
| `plasma-resolve` | A colorful plasma field (LUT-based) over the bg resolves/condenses into the logo as t→1 | high | rejected 2026-06-12 (Fab) |
| `rotate-step` | Logo snaps through 90°-step rotations (pixel-art-safe) while scaling up into place | low | rejected 2026-06-12 (Fab) |
| `mosaic-shrink` | Logo rendered at coarse block quantization (block size shrinks 12→1 src px) with alpha fade | low | implemented (see above) |
| `diamond-wipe` | L1 (Manhattan) distance reveal — diamond-shaped iris growing from bbox center | low | implemented (see above) |
| `flood-fill` | BFS through 4-connected opaque region from a deterministic seed; topology-aware reveal | medium | rejected 2026-06-12 (Fab) |
| `pixel-shuffle` | Per-pixel 2D random sample offset that decays to 0; logo "unscrambles" in place | low | rejected 2026-06-12 (Fab) |
| `shutter-bands` | Vertical column-bands open from bbox center outward in band-distance order | low | rejected 2026-06-12 (Fab) |
| `color-emerge` | Silhouette in contrast color fades in, then per-channel value lerps from silhouette to source color | low | implemented (see above) |
| `starburst` | Per-pixel fly-in along home-radial direction with back-out (overshoot) easing | medium | implemented (see above) |
| `fire-burnup` | Stateless fire-noise field thresholded on time; flames die away revealing logo | low | rejected 2026-06-12 (Fab) |
| `plasma-dissolve` | Smooth plasma field used as dissolve threshold (correlated noise vs hash) | low | implemented (see above) |
| `ripple-converge` | 2D radial sinusoidal sample displacement, smoothstep-damped amplitude | low | rejected 2026-06-12 (Fab) |
| `voronoi-shatter` | Voronoi cells fly in radially from outside; per-cell easing | medium | implemented (see above) |
| `hue-cycle-lock` | Per-pixel HSV hue rotation decaying to 0 at t=1 | low | implemented (see above) |
| `twirl-unwind` | Photoshop-style spiral warp with radius-dependent rotation; unwinds | medium | rejected 2026-06-12 (Fab) |
| `bayer-reveal` | 8×8 ordered Bayer matrix as threshold field; deterministic crosshatch reveal | low | rejected 2026-06-12 (Fab) |
| `rotozoom-settle` | Affine inverse-mapping (rotation+scale) decaying to identity | medium | rejected 2026-06-12 (Fab) |

Selection guidance when picking batches: favor variety of *mechanism*
(dissolve vs motion vs wipe vs distortion vs zoom) over variety of theme,
so the final 12 feel genuinely different at a glance. Keep at most one
`high` perf-risk concept per batch so device round-trips aren't dominated
by optimization work.
