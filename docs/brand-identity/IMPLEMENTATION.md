# Brand themes — implementation notes

All five brand directions from this folder ship as **live, switchable themes** in the web UI
(`spectrum` · `gallery` · `console` · `dither` · `blossom`). This file documents how the system
works and how to extend it.

> **Spectrum vs Pixel Console** are deliberately pushed to opposite poles so they don't read alike:
> Spectrum = deep-violet canvas, rounded, flat, multi-colour, a rainbow (conic-gradient) wordmark,
> sans body; Pixel Console = pure cool-black CRT, sharp corners, scanlines, monochrome cyan glow,
> all-mono uppercase wordmark.

## How it works

- **Selection mechanism.** The active theme is a `data-theme` attribute on `<html>`
  (`spectrum` · `gallery` · `console` · `dither` · `blossom`). `webui/static/common.css` defines every
  colour/type/radius as a **design token** (CSS custom property); `:root` holds the default
  (Spectrum) and each `html[data-theme="…"]` block overrides the tokens.
- **Persistence.** `webui/static/theme.js` reads/writes `localStorage["p3a-theme"]`
  (default `spectrum`). It is loaded **synchronously in each page's `<head>`** so the saved
  theme is applied before first paint (no flash) and keeps `<meta name="theme-color">` in sync.
  It renders no UI — it exposes `window.p3aTheme = { THEMES, get(), set(id) }`.
  > Per-browser today. A future revision may promote this to an NVS device setting — at that
  > point `theme.js` should hydrate the initial value from `/config` and POST changes back,
  > keeping `localStorage` as the offline fallback.
- **Picker.** A `<select>` in the **Settings → Display tab** ("Theme" section, styled like the
  other Display sections). It is populated from `window.p3aTheme.THEMES`, and `onchange` calls
  `window.p3aTheme.set(id)`. (There is no global footer picker.)

## Files touched

| File | Role |
|------|------|
| `webui/static/common.css` | Token vocabulary + 4 theme blocks + per-theme signature rules (pixel grid, console glow, dither dot-screen, wordmark voice) + picker styling |
| `webui/static/theme.js` | **New.** Pre-paint apply, persistence, `window.p3aTheme` API (no UI) |
| `webui/settings.html` | Hardcoded colours → tokens; `theme.js` in head; **Theme** picker section in the Display tab |
| `webui/index.html`, `playset-editor.html`, `ota.html`, `pico8/index.html` | Hardcoded colours → tokens; `theme.js` in head |
| `webui/museum/browse.js` | Injected museum-browse modal retargeted to card tokens |
| `CMakeLists.txt` | `WEBUI_VERSION` 2.11 → 2.12 |

`version.txt` and `metadata.json` are **build-generated** from `WEBUI_VERSION` — do not edit them
by hand. `compat.js` is generated from `compat.js.in`.

## Token families (see the header comment in common.css for the full list)

- `--c-bg` / `--c-on-bg*` / `--c-bg-fill*` — the page canvas and things directly on it
- `--c-card-*` / `--c-surface-sunk` / `--c-input-bg` — the raised (card/panel/input) world
- `--c-text* / --c-border` — **legacy aliases** of the card family (so old `var(--c-text)` refs
  theme for free; defined once in `:root`, they re-resolve per theme via `var(--c-card-fg)`)
- `--c-primary` / `--c-accent` (+ `--c-on-*` for text on them) — brand + action
- `--c-success/danger/warning` (+ `-dark`, `--c-on-*`, and `*-soft-bg/-fg` pairs) — state
- `--c-pill-user/pin/builtin` (+ `-fg`) — the playlist-pill families
- `--font` / `--font-display` / `--font-mono`, and `--r-sm/md/lg`

Translucent tints are derived at use-site with `color-mix(in srgb, var(--c-…) N%, transparent)`
(widely supported in current mobile browsers), so a single base token drives both the solid and
the tinted states.

## Conventions that keep colour disciplined

- **On-canvas vs in-card.** Anything on the body background uses the `--c-on-bg*` / `--c-bg-fill*`
  family; anything inside a card/modal uses `--c-card-*`. This is what lets light themes (Gallery,
  Dither) and dark themes (Spectrum, Console) both stay legible from the same markup.
- **Text-on-fill tokens.** Buttons/badges read their text colour from `--c-on-primary`,
  `--c-on-success`, etc. — never a hardcoded `white`, because on Gallery's amber or Console's lime
  the correct text is dark.
- **Theme-safe literals left alone:** black-alpha drop shadows / scrims, modal backdrops, the
  categorical `channelColors` chart palette in playset-editor, and the live RGB preview swatch.

## Adding another theme

1. Add an `html[data-theme="<id>"] { … }` block in common.css with the full token set.
2. (Optional) add a signature rule block at the bottom for texture/wordmark.
3. Add `{ id, name }` to `THEMES` and an entry to `THEME_COLOR` in `theme.js`.

## Out of scope (intentionally not themed)

The Wi-Fi **setup / captive-portal** pages (`webui/setup/*`) are standalone, served pre-network in
AP mode, and do not load `common.css`/`theme.js`. They keep their own minimal styling.
