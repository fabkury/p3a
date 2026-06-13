# p3a Web UI — Current-State Audit (fresh eyes)

*A first-principles read of what the UI says today, before proposing anything new.
Written 2026-06-13. Sources: `webui/static/common.css`, `webui/index.html`,
`webui/settings.html`, `webui/setup/index.html`, the PWA icons in `webui/static/`.*

## What the UI is built from today

| Layer | Current value | What it signals |
|-------|---------------|-----------------|
| **Primary brand color** | `#667eea` (periwinkle indigo) | — |
| **Brand gradient** | `linear-gradient(135deg, #667eea 0%, #764ba2 100%)` | the single most over-used "startup/template" gradient on the web (CodePen-demo lavender→purple). It is generic and, crucially, **muted** — it actively contradicts the stated brand value of *colorfulness*. |
| **Surfaces** | `rgba(255,255,255,0.95)` + `backdrop-filter: blur(8px)` | glassmorphism — frosted white cards floating on the gradient. A dated, template-y look. |
| **Radii** | 6 / 10 / 16 px | soft, friendly, "app-ish" — bubbly rather than crisp/pixel. |
| **Typography** | system stack only (`-apple-system, Segoe UI, Roboto`) | no typographic identity at all. The wordmark is just thin (`font-weight:300`) lowercase letter-spaced text. |
| **Semantic colors** | success `#10b981`, danger `#ef4444`, warning `#ff9800` | standard Tailwind-ish defaults — fine, but unrelated to any brand system. |
| **Pill category accents** | user = amber `#f4c44a`, pin = pink `#ec4899`, builtin = sky `#38bdf8`, utility = neutral | **the single most on-brand asset in the codebase** — four distinct vivid hues used systematically. But they were chosen ad-hoc and aren't tied to the gradient or any palette. |
| **App icon / "logo"** | a 3D render of the device (cream casing, **dark-blue** screen) on black | does *not* match the brand the project describes (a pixel-art device showing a **high-saturation hue wheel** + the letters "p3a"). The screen is rendered dead blue — the opposite of colorful. |

## The core problems

1. **The gradient is anti-brand.** A muted lavender→purple says "generic SaaS template." The
   brand wants to say "the ultimate pixel-art player, colorfulness appreciated." These fight.
2. **No typographic voice.** A *pixel-art* player has the most obvious type opportunity
   imaginable (a bitmap / mono / display face for the wordmark and readouts) and uses none of it.
3. **The logo isn't real yet.** The described mark — pixel device + hue wheel + "p3a" — exists as
   a *concept* but the shipped icons are an unrelated 3D render with a blue screen. There is no
   reusable wordmark, glyph, or color system derived from the hue wheel.
4. **The two poles aren't expressed.** p3a is simultaneously (a) a playful, vivid pixel-art / Giphy
   player and (b) a sober, high-res IIIF museum display. The current UI is neither — it's a flat
   template that serves the artwork without any point of view about *either* identity.
5. **Glassmorphism dates the product.** Frosted cards on a gradient was a 2020–2021 trend.
6. **Color is decorative, not systematic.** Where color appears (pills, banners) it's locally
   invented. There's no palette anyone could extend predictably.

## The opportunities (what's actually good / latent)

- **The hue wheel is a gift.** A logo that is literally a color wheel hands you a complete,
  harmonious, *systematic* palette for free — spectral stops at fixed saturation/lightness are
  guaranteed to harmonize.
- **The four pill hues already prove** that disciplined, category-mapped color reads as organized
  rather than tiring. That principle should be elevated to the whole system.
- **The device is a screen.** Pixel art and 24-bit IIIF scans both look their best on **dark,
  neutral "display-off"** backgrounds — the product itself argues for a dark-canvas option.
- **Two poles → two modes.** The vibrant-vs-sober tension is not a problem to average away; it's a
  natural **dual-mode** system (a colorful "player" mode and a calm "gallery" mode) sharing one
  skeleton.

## The tension to resolve, stated plainly

> *Colorfulness is a brand value, but undisciplined color is tiring and looks disorganized.*

Every proposal in this folder resolves that tension a **different** way, so the choice is real:

- **Spectrum** — color is *systematic* (one spectral wheel, neutral everything else).
- **Gallery** — color is *reserved for the art* (near-monochrome chrome).
- **Pixel Console** — color is *emissive and sparse* (a few lit accents on dark instrument chrome).
- **Dither Pop** — color is *limited by a print palette* (3–4 riso inks, restraint by constraint).
