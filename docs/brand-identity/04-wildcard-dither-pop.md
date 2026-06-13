# Proposal D (wildcard) — **Dither Pop** (Risograph)

> *Few inks, printed bold. The dither pattern is pixel art's grandparent.*

## Concept

A print-poster identity: a tight, punchy **risograph** ink palette on warm paper, bold geometric
type, flat color blocks, and a **1-bit ordered-dither / halftone** texture as the signature motif.
That motif is the conceptual hook — *dithering is how limited palettes faked more colors*, which is
exactly the lineage of the pixel art p3a exists to play. The brand's heritage becomes its texture.

This is the most **opinionated**, design-forward, marketing-friendly direction — it would look as
good on a poster, a sticker, or a box as on the screen.

**How it resolves the tension:** restraint by **constraint**. A risograph press runs a handful of
inks; that limit makes the result colorful *and* tasteful automatically. You get loud, joyful color
and disciplined cohesion at the same time — because there are only four inks and they overprint.

## Who it's for

The design-literate audience; anyone who'd appreciate p3a as a *designed object* with a strong
visual point of view. Excellent for the project's outward face (README, store page, packaging,
the `docs/outreach/` materials) as well as the UI.

## Palette

### Riso inks on paper (light / "day")

| Token | Hex | Use |
|-------|-----|-----|
| `--paper`      | `#F3EEE3` | warm newsprint — background |
| `--ink`        | `#1A1A1A` | near-black riso black — text, rules |
| `--riso-blue`  | `#2541B2` | federal blue — **primary action** (replaces `#667eea`) |
| `--riso-red`   | `#F23030` | fluoro red/coral — **danger / emphasis** |
| `--riso-yellow`| `#FFC400` | sunny yellow — **warning / highlight** |
| `--riso-green` | `#00A364` | **success** (5th ink, optional) |
| `--riso-pink`  | `#FF48A0` | fluoro pink — the playful pop / pins |

### Dark ("night press")

| Token | Hex |
|-------|-----|
| paper | `#161412` |
| ink | `#EDE7D9` |
| inks | brightened riso values (blue `#5B79FF`, red `#FF5A4D`, yellow `#FFD23D`, pink `#FF6FB3`) |

### Mapping onto `common.css`

```text
--c-primary     → --riso-blue #2541B2
--c-gradient    → (removed) flat --paper; depth comes from DITHER, not gradients
--c-card-bg     → --paper (cards differ from bg by a 2px --ink keyline, not shadow)
--c-card-shadow → none → replaced by a hard offset block ("overprint" shadow) where needed
--c-success/danger/warning → --riso-green / --riso-red / --riso-yellow
--c-text        → --ink #1A1A1A
```

## Typography

**Bold grotesque / geometric** display. Heavy weights, tight tracking, big sizes — poster energy.
- **Wordmark "p3a":** a solid color-block lockup; the "3" can be a knockout (reversed paper inside
  an ink block). Confident and graphic.
- **UI / body:** a clean grotesque (system: `'Helvetica Neue', Arial, sans-serif`) or the existing
  system sans at heavier weights for labels.

## Surface & treatment language — the signature

- **Flat color blocks. No gradients. No soft shadows** (a printing press has none).
- **Depth via overprint:** elements get a hard **offset block** behind them (e.g., a 4px solid
  `--ink` or second-ink offset) — the classic riso misregistration look — instead of a blur shadow.
- **Dither as shading:** anywhere you'd normally use a gradient or a tint (progress fill,
  hover wash, disabled state, hero panels), use an **ordered-dither (Bayer) pattern** as a CSS
  background. This is the brand's fingerprint and a literal pixel-art technique.
- **Radius near zero** (0–4px) — printed blocks, not bubbles. Thick `--ink` rules separate sections.

## The two poles

- **Pixel-art / Giphy / player:** full multi-ink riso — loud and joyful.
- **Museum / IIIF:** a **monochrome riso** treatment — single ink (`--riso-blue` or `--ink`) on
  paper with dither shading. Reads as a refined *exhibition poster*, which is a credible and
  tasteful museum register. The dither even references engraving/halftone reproduction of artworks.

## Motion personality

Minimal and graphic: hard cuts, block wipes, a "print-registration" snap on state change. The dither
pattern can shift one step on hover (a tactile "re-screen"). Nothing floats or fades softly.

## Component sketches

- **Primary button:** solid `--riso-blue` block, `--paper` text, 0–2px radius, hard `--ink` offset
  shadow on hover; active removes the offset (button "presses into" the page).
- **Pills:** bold flat ink chips; categories = different inks (user yellow, pin pink, builtin blue,
  utility outlined ink). Active = filled + offset block.
- **Progress / now-playing:** dither-pattern fill rather than a smooth gradient bar.
- **Banners (update / cooldown):** full-bleed single-ink blocks with knockout text — very poster.

## Pros / cons

**Pros**
- Most distinctive and ownable; the dither motif ties cleverly to pixel art's low-bit-depth roots.
- Limited palette is inherently tasteful while still loud — directly threads the brief's needle.
- Travels brilliantly off-screen (packaging, stickers, README, outreach posts).

**Cons / risks**
- The most divisive / opinionated — a strong aesthetic some users won't love.
- Flat print look can feel less "soft/interactive"; needs care so controls still read as tappable.
- Dither textures and overprint offsets must be tuned for a small embedded UI and for accessibility
  (contrast of dithered text areas).
- Furthest from a conventional "settings app" feel — higher design-confidence required to pull off.
