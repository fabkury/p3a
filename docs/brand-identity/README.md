# p3a — Brand & Visual Identity

*Created 2026-06-13. The directions below began as proposals; **all now ship as live, switchable
themes** in the web UI, selectable from **Settings → Display → Theme**. See
[`IMPLEMENTATION.md`](IMPLEMENTATION.md) for how the system is built and extended. The proposal
docs are kept as the design rationale behind each theme.*

## The brief, distilled

p3a is, before anything else, **the ultimate pixel-art player**. Its logo is a pixel-art rendering
of the device showing a **high-saturation hue wheel**, with the letters **p3a** beneath. From that
follow the brand's twin commitments and the tension between them:

- **Colorfulness is a value** — muted tones are off-brand.
- **…but balanced with taste** — undisciplined color is tiring and looks disorganized.
- p3a also has a **sober pole**: high-res IIIF **museum** artworks on a 720×720 panel.
- The center of gravity remains **pixel art**.

## What's in this folder

| File | Contents |
|------|----------|
| [`00-current-state-audit.md`](00-current-state-audit.md) | Fresh-eyes critique of today's UI and where it falls short |
| [`01-spectrum.md`](01-spectrum.md) | **Proposal A — Spectrum**: the hue wheel *is* the system |
| [`02-gallery.md`](02-gallery.md) | **Proposal B — Gallery**: a museum on your wall; art is the only color |
| [`03-pixel-console.md`](03-pixel-console.md) | **Proposal C — Pixel Console**: emissive instrument/HUD chrome |
| [`04-wildcard-dither-pop.md`](04-wildcard-dither-pop.md) | **Proposal D (wildcard) — Dither Pop**: risograph inks + 1-bit dither |
| [`05-blossom.md`](05-blossom.md) | **Proposal E — Blossom**: a moderately feminine soft-blush theme |
| [`IMPLEMENTATION.md`](IMPLEMENTATION.md) | How the live theme system is built and extended |

## The one thing wrong today

The current UI runs on `linear-gradient(135deg, #667eea, #764ba2)` — the internet's most generic
"template lavender" gradient — under frosted-glass cards and a thin, voiceless system font. It is
*muted* (anti the colorfulness value), *generic* (no point of view), and the shipped app icon is a
3D device render with a **dead-blue** screen that doesn't even match the described hue-wheel logo.
There is, effectively, **no brand yet**. Full detail in the audit.

## How each proposal resolves the colorful-vs-tasteful tension

This is the real decision. Each direction answers it differently, on purpose:

| Proposal | Color philosophy | One-line feel | Leans toward |
|----------|------------------|---------------|--------------|
| **A · Spectrum** | Color is **systematic** — one 12-stop spectral wheel, neutral chrome | Playful, engineered, "the logo, alive" | Pixel-art player |
| **B · Gallery** | Color is **reserved for the art** — near-monochrome warm chrome | Calm, premium, editorial | Museum / IIIF |
| **C · Pixel Console** | Color is **emissive & sparse** — lit accents on black | Techy, retro-future, hardware | The device itself |
| **D · Dither Pop** | Color is **limited by a print palette** — 3–4 riso inks | Bold, graphic, poster-like | Pixel-art heritage |

## Comparison at a glance

| Dimension | A · Spectrum | B · Gallery | C · Pixel Console | D · Dither Pop |
|-----------|:---:|:---:|:---:|:---:|
| On-brand with the hue-wheel logo | ●●● | ● | ●● | ●● |
| Serves the museum/IIIF pole | ●● (sub-mode) | ●●● | ●● (Exhibit mode) | ●● (mono mode) |
| Colorful | ●●● | ● | ●● | ●●● |
| Tasteful / restrained | ●● | ●●● | ●● | ●● |
| Distinctiveness (not generic) | ●● | ●● | ●●● | ●●● |
| Implementation effort | ●● | ● (lowest) | ●●● | ●●● (highest) |
| Risk of aging / trend-dependence | low | very low | medium | medium |
| Default canvas | dark | light | dark | light |

## Cross-cutting principles (apply to whichever direction wins)

1. **Kill the lavender gradient.** Whatever replaces it, the `#667eea→#764ba2` wash must go — it is
   the single biggest source of the "generic template" read.
2. **Color is systematic or it is restrained — never decorative.** Map every color to a *role*
   (state, category, action) so colorfulness reads as organized, not noisy.
3. **Give the wordmark a voice.** A pixel-art player should not be set in default system thin text.
4. **Make the logo real and reusable.** Turn the described hue-wheel-device mark into an actual
   glyph + wordmark + palette, and reconcile the shipped icons with it (today's blue-screen render
   contradicts it).
5. **Plan for two poles.** Ship a vibrant context and a calm context — as a mode toggle, not two
   codebases.
6. **Respect the constraints.** Assets live in a 4 MB LittleFS partition; prefer system/variable
   fonts and CSS-generated texture over heavy webfonts and bitmaps where possible.

## A meta-recommendation

The vibrant-vs-sober tension is **not a problem to average away — it's a dual-mode system.** The
strongest outcome is likely **one identity with two modes**:

> **Recommended: Spectrum (Proposal A) as the flagship "Player" mode, with Gallery (Proposal B) as
> its "Gallery" mode** — a single toggle, shared skeleton.

Rationale:
- **Spectrum** is the most faithful expression of the stated mission and the literal logo, and it
  turns the hue wheel into a real, extensible color system (the four existing pill hues already
  prove the approach works).
- **Gallery** is the most credible answer to the museum/IIIF pole, and it slots in cleanly as
  Spectrum's calm mode rather than a competing design language.
- Together they let p3a be *unapologetically colorful when playing pixel art* and *quietly elegant
  when showing a Vermeer* — which is exactly what the brief describes.

**Pixel Console** and **Dither Pop** are the bolder, higher-personality alternatives — pick one of
them instead if the goal is maximum distinctiveness and a single strong attitude over the
two-mode flexibility.

*No code has been changed. Pick a direction (or a combination) and the next step is a token sheet +
one reference screen built for review.*
