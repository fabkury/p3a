# Proposal A — **Spectrum**

> *The logo is a hue wheel. So is the brand.*

## Concept

Lean all the way into the thing that already defines p3a: the logo's high-saturation hue wheel.
**Spectrum** turns that wheel into the entire color system — a 12-stop spectral ring at one fixed
saturation and lightness, so every brand color is a sibling of every other and they *cannot* clash.
The chrome around the color is pushed to a deep "display-off" near-black, so accents and artwork
glow the way pixel art does on the device's 24-bit IPS panel.

This is the most direct expression of the stated mission — *the ultimate pixel-art player,
colorfulness appreciated* — and the most direct expression of the actual logo.

**How it stays tasteful, not tiring:** the colorfulness is *systematic*. Twelve hues share identical
S/L, so the palette is harmonious by construction. The discipline rule is **"one accent per
context, neutral everything else."** A screen is mostly near-black ink; color marks *state* and
*category* only. The riot of color lives inside the artwork frame — exactly where it belongs.

## Who it's for

The maker/enthusiast who bought a *pixel-art player* and wants it to feel like one — playful,
energetic, unmistakably about color, but engineered and orderly rather than chaotic.

## Palette

### Neutrals — "display-off" ink (dark-first)

| Token | Hex | Use |
|-------|-----|-----|
| `--ink-900` | `#0B0B0F` | canvas / body background (the screen, off) |
| `--ink-800` | `#14141B` | panel surface |
| `--ink-700` | `#1E1E28` | raised surface / inputs |
| `--ink-600` | `#2A2A38` | borders, 1px keylines |
| `--ink-400` | `#6B6B82` | disabled / faint |
| `--ink-300` | `#9A9AB0` | secondary text |
| `--ink-100` | `#E8E8F0` | primary text on dark |
| `--paper`   | `#FAFAFC` | light-mode surface (see Light mode) |

### The Spectrum — 12 stops, ~S85 / L62, evenly 30° apart

| Token | Hue | Hex | Default role |
|-------|-----|-----|--------------|
| `--h000` | red       | `#FF4D4D` | **danger** |
| `--h030` | orange    | `#FF8A33` | **warning** |
| `--h060` | amber     | `#FFC533` | pill: user playsets |
| `--h090` | lime      | `#B6E335` | — |
| `--h120` | green     | `#42D86E` | **success** |
| `--h150` | spring    | `#26D9A3` | — |
| `--h180` | cyan      | `#2BD6D6` | — |
| `--h210` | azure     | `#33A6FF` | **primary action** (replaces `#667eea`) |
| `--h240` | blue      | `#5C6BFF` | — |
| `--h270` | violet    | `#9A5CFF` | pill: builtin channels |
| `--h300` | magenta   | `#E84DD6` | — |
| `--h330` | rose      | `#FF4D97` | pill: pins |

### Mapping onto the existing `common.css` tokens (drop-in)

```text
--c-primary       → --h210  #33A6FF
--c-primary-dark  → #1F86E0
--c-gradient      → conic-gradient(from 0deg, the 12 stops)   /* the hue wheel — used ONLY for the
                                                                  wordmark ring + loading spinner */
--c-success       → --h120  #42D86E
--c-danger        → --h000  #FF4D4D
--c-warning       → --h030  #FF8A33
--c-text          → --ink-100 (dark mode) / #1A1A22 (light)
--c-card-bg       → --ink-800 (dark) / --paper (light)
```

The brand gradient stops being a *background wash* and becomes a *conic rainbow* used **only** as
the literal hue-wheel motif (wordmark ring, loading spinner). Backgrounds are flat ink.

## Typography

- **Wordmark + accents:** a chunky **bitmap / pixel** display face (or a geometric mono) for "p3a".
  The pixel grid of the letters *is* the brand. The "3" can carry a subtle hue tint.
- **UI text:** keep a clean system sans for body/labels — legibility first.
- **Numerals & readouts** (counts, intervals, version): a tabular **mono** so device data reads as
  device data.

## Logo / wordmark treatment

A small **12-segment pixel hue-ring** (8×8 or 12-wedge) sits as the brand glyph — directly the
described logo. The wordmark "p3a" sits beneath in the pixel face. The ring doubles as the app's
**loading spinner** (it rotates / the active wedge cycles through hues), so the logo is *alive*
throughout the UI.

## Surface & treatment language

- **Flat panels**, not glass. 1px `--ink-600` keylines instead of blur + drop shadow.
- **Crisp radii**: `--r-sm 4px`, `--r-md 8px` (pixel-ish, not bubbly). Pills stay rounded as the
  one soft exception (they're chips).
- Optional **1px pixel-grid** texture on the canvas at very low contrast (≈ `--ink-800` lines on
  `--ink-900`) — a quiet nod to the display matrix.
- **Selection / active** = solid hue fill + a 2px hue ring (`box-shadow: 0 0 0 2px <hue>88`). This
  generalizes the pattern the pill bar already uses today.

## The two poles

Spectrum ships a **Gallery sub-mode** (toggle) for the museum/IIIF context: chrome desaturates to
pure neutral ink, all spectral accents collapse to a single quiet accent, and the artwork gets one
warm-white keyline frame. Same skeleton, calm clothes. (If you prefer a fuller treatment of the
sober pole, that's Proposal B — *Gallery* — which can be adopted as Spectrum's calm mode wholesale.)

## Motion personality

**Stepped, not smooth.** Use quantized easing (`steps()`, frame-snapped transitions) so motion
echoes the device's frame-based animation player. The hue-ring spinner advances one wedge at a time.
Snappy, ≤200 ms, deliberately "digital."

## Component sketches

- **Primary button:** flat `--h210` fill, ink text, 8px radius; active = `steps()` scale-down + ring.
- **Pills:** keep the four-family idea but re-tinted to spectral stops (user `--h060`, pin `--h330`,
  builtin `--h270`, utility neutral). Active = solid stop + 2px ring of the same hue.
- **Now-playing bar:** `--ink-800` panel, hue-ring spinner when refreshing, mono stats.
- **Artwork frame:** thin `--ink-600` keyline on `--ink-900`; art sits on near-black so 24-bit color
  pops. (This is the moment the whole "display-off canvas" idea pays off.)

## Pros / cons

**Pros**
- The most on-brand option — it *is* the logo, made into a system.
- Colorful **and** organized; the harmony is mathematical, not a matter of taste.
- Dark canvas flatters both pixel art and high-res IIIF scans.
- The four existing pill hues already validate the approach.

**Cons / risks**
- Dark-first may feel less "friendly-app" to some users → ship a light mode (`--paper`).
- Requires a pixel/mono font asset to hit the wordmark concept (LittleFS budget consideration).
- Needs discipline in code review — without the "one accent per context" rule it could drift toward
  carnival.
