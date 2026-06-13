# Proposal E — **Blossom**

> *Soft, warm, and quietly elegant — colour as a boutique, not a carnival.*

## Concept

A fifth, **moderately feminine** theme. Not hot-pink or saccharine — a tasteful boutique palette:
a soft **blush** canvas, **plum** ink, a confident **rose** primary with a **lilac** accent, and an
**elegant italic serif** wordmark. It reads soft and refined rather than loud, and it gives the
lineup a warm, gentle register that none of the other four occupy.

**How it resolves the colourful-vs-tasteful tension:** warmth and softness rather than vividness.
The hues are desaturated toward pastel, the corners are generously rounded, and the type is
graceful — the result is colourful in feeling without being bright or busy. Restraint via *softness*.

## Who it's for

Someone who wants p3a to feel pretty and personal on a shelf or vanity — a softer, more decorative
register than Spectrum's energy or Gallery's museum sobriety.

## Palette

### Canvas & surfaces (light)

| Token | Hex | Use |
|-------|-----|-----|
| `--c-bg`         | `#FBF1F3` | soft blush canvas |
| `--c-on-bg`      | `#4A2540` | plum/aubergine ink (primary text) |
| `--c-on-bg-muted`| `#8A6A7E` | muted mauve |
| `--c-card-bg`    | `#FFFBFC` | near-white card |
| `--c-card-border`| `#F0DAE1` | soft pink hairline |
| `--c-card-shadow`| `0 6px 20px rgba(209,78,140,0.16)` | a gentle rosy lift |
| `--c-surface-sunk`| `#FBEEF2` | sunk panels / inputs |

### Brand / accent / state

| Token | Hex | Role |
|-------|-----|------|
| `--c-primary` | `#D14E8C` | rose (raspberry, not neon) |
| `--c-accent`  | `#8E5BA6` | lilac-plum |
| `--c-gradient`| `linear-gradient(135deg, #D14E8C, #8E5BA6)` | rose → lilac |
| success | `#46A688` (soft teal-green) | — |
| danger  | `#D2425C` (rosy red) | — |
| warning | `#E0993D` (warm amber) | — |
| pills | user amber `#E6A95C` · pin rose `#D14E8C` · builtin lilac `#8E5BA6` | — |

### Type & shape

- **Wordmark / display:** an elegant high-contrast serif (`'Didot', 'Bodoni MT', 'Iowan Old Style',
  Georgia, serif`), set **italic** — a fashion-plate touch. UI body stays a clean system sans.
- **Radii:** the softest in the set — `8 / 14 / 22 px`. Everything is gently rounded.
- **Signature:** a faint blush radial wash on the canvas (rose top, lilac lower-left).

## Pros / cons

**Pros**
- Fills a warm, soft register the other four lack; broadens the lineup's appeal.
- "Moderate" by design — refined and tasteful, not a stereotype.

**Cons / risks**
- The serif display relies on a system serif stack that varies across platforms.
- Soft pastel contrast needs watching for legibility (text tokens are kept dark plum to compensate).
