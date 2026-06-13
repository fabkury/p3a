# Proposal B — **Gallery** (Passe-partout)

> *A tiny museum on your wall. The chrome steps back; the art is the only hero.*

## Concept

p3a's most aspirational feature is showing high-res IIIF museum artworks on a 720×720 panel.
**Gallery** treats the whole web UI as the *gallery itself*: a calm, warm-neutral wall; a generous
**passe-partout** (the mat board around a framed print); editorial typography; and color held in
reserve the way a good museum reserves it — for the work, not the walls.

This is the deliberate opposite pole from *Spectrum*. Where Spectrum makes the chrome colorful,
Gallery makes the chrome near-silent so that the artwork — whether a vivid pixel GIF or a Rembrandt
scan — supplies **all** the color in the room.

**How it resolves the tension:** by refusing to compete. "Colorfulness appreciated" is honored
inside the artwork frame; "balanced with taste" is honored everywhere else. Restraint *is* the
brand.

## Who it's for

Someone who hangs p3a on the wall as an object of taste — the IIIF / museum audience, the
design-conscious buyer, the "I want it to look like a gallery label, not a gadget" user.

## Palette

### Warm museum neutrals (light / "day")

| Token | Hex | Use |
|-------|-----|-----|
| `--mat`    | `#F4F1EA` | passe-partout cream — body background |
| `--wall`   | `#FBFAF7` | gallery wall — cards / surfaces |
| `--wall-2` | `#ECE7DD` | sunk panels / inputs |
| `--line`   | `#DAD3C6` | hairline rules & frame keylines |
| `--label`  | `#6B6357` | gallery-label warm gray (secondary text) |
| `--ink`    | `#2A2620` | warm near-black (primary text) |

### Reserved accents (used sparingly)

| Token | Hex | Use |
|-------|-----|-----|
| `--accent`      | `#244E66` | deep museum/Prussian blue — primary action, links (replaces `#667eea`) |
| `--accent-deep` | `#18384A` | pressed / hover |
| `--gilt`        | `#B08A3E` | restrained brass — pins, "featured", frame highlight. **Very** sparing. |

### Museum-toned semantics (muted, never neon)

| Token | Hex | vs. today |
|-------|-----|-----------|
| success | `#3E7A53` (sage) | was `#10b981` |
| danger  | `#9B3B33` (oxblood) | was `#ef4444` |
| warning | `#B8862E` (ochre) | was `#ff9800` |

### Gallery Night (dark / "after hours")

| Token | Hex |
|-------|-----|
| bg | `#14120E` (warm near-black) |
| surface | `#1E1B15` |
| line | `#363025` |
| text | `#EDE7D9` |
| accent | `#4E8FB0` (brightened blue) |
| gilt | `#C9A24E` |

Dark galleries are real and flatter high-res art beautifully — Gallery Night doubles as the
"display on the wall at night" mode.

### Mapping onto `common.css`

```text
--c-primary      → --accent  #244E66
--c-gradient     → (removed) flat --mat; NO gradient wash
--c-card-bg      → --wall    #FBFAF7   (opaque paper, no blur)
--c-card-shadow  → none → replaced by 1px --line hairline rules
--c-text         → --ink     #2A2620
--c-success/danger/warning → museum-toned values above
```

## Typography

An **editorial** voice. Pairing concept:
- **Headings / wordmark:** a refined serif feel (transitional/humanist) for an "exhibition catalog"
  tone. On an embedded UI, approximate with a tasteful serif system stack (`Georgia, 'Iowan Old
  Style', 'Times New Roman', serif`) rather than shipping a webfont.
- **UI / body:** the existing system sans, but quieter — more letter-spacing on labels, lighter
  weight.
- **Artwork captions** become **museum labels**: small-caps keys (ARTIST / TITLE / DATE) stacked,
  hairline rule above, in `--label` gray. The metadata panel that already exists in `index.html`
  (`#artwork-info-*`) becomes the showpiece.

### Wordmark

"p3a" set in small-caps serif with refined letter-spacing — the *only* playful note is the "3".
Quiet, confident, gallery-signage energy. No glyph required; the typography *is* the mark.

## Surface & treatment language

- **The passe-partout frame** is the signature: artwork sits inside a generous mat (`--mat`) with a
  thin **inner keyline** (`--line`) a few px off the image — exactly like a real picture mat. This
  one detail carries the whole identity.
- **Paper, not glass.** Cards are opaque `--wall` separated by **hairline rules**, not shadows and
  blur. Optional faint paper grain.
- **Minimal radius** (4–6px) — restrained, architectural, not bubbly.
- Generous **whitespace**; the layout breathes like a wall with one work per bay.

## Motion personality

Slow, refined cross-fades (no bounce, no scale-pop). Artwork transitions ease like a gallery
slideshow. Nothing calls attention to itself. ~300–400 ms, gentle ease.

## Component sketches

- **Primary button:** `--accent` fill, `--wall` text, 5px radius, no shadow; hover → `--accent-deep`.
- **Pills:** flatten to **label chips** — `--wall-2` fill, `--line` border, `--ink` text; active =
  `--accent` underline or fill. The four categories differ by a small left keyline color, not loud
  fills.
- **Now-playing:** a discreet `--wall` strip with a hairline top rule; stats in `--label`.
- **Museum frame / metadata:** the hero. Mat + inner keyline + small-caps caption block beneath.

## Pros / cons

**Pros**
- Best expression of the IIIF/museum aspiration; premium and timeless.
- Makes the artwork the unambiguous hero — strong for both pixel art *and* fine-art scans.
- Ages well; no trend dependency.

**Cons / risks**
- Least "colorful" — risks feeling too sober for the playful Giphy / pixel-art-player side.
- Leans on editorial type discipline; a serif system stack varies across platforms.
- The brand value "colorfulness appreciated" is honored only indirectly (via the art), which may
  feel like an under-commitment to the project's stated personality.

> **Pairing note:** Gallery is the natural **calm mode** for *Spectrum* (Proposal A). Shipping
> Spectrum-as-"Player" + Gallery-as-"Gallery" — one toggle — may be the strongest overall outcome.
> See `README.md` → "A meta-recommendation."
