# docs/klipy

Evaluation of [Klipy](https://klipy.com/) (a Giphy/Tenor-style GIF · Sticker · Clip API) as a
potential p3a artwork source alongside Makapix, Museums, and Giphy.

- **[klipy-integration-evaluation.md](klipy-integration-evaluation.md)** — full feasibility &
  integration report. All findings **live-verified against the Klipy API on 2026-07-01**.

**TL;DR:** Feasible and low-risk — architecturally a near-mirror of `components/giphy/`, and actually
*simpler* (Klipy returns explicit per-rendition URLs, so the Giphy CDN-filename/`downsized_medium`
guessing disappears). The one design wrinkle (opaque CDN URLs) is solved by storing the compact
numeric item `id` and re-resolving `gifs/{id}` at download time. The strongest reason to build it is
**Stickers** (transparent *animated* WebP — a new content type p3a already decodes) and **Giphy
provider resilience**, not "more GIFs." Estimated ~2–3 weeks for GIF parity, +~3–5 days for Stickers.

Status: decision-support only — no code written. Open question is scope (Stickers / fallback / just
exploring); see §8 of the report.
