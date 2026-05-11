# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: museum-art

A unified browsing interface that abstracts diverse museum APIs behind a single client-facing surface. Scope is restricted to museums that expose artworks via **IIIF** (International Image Interoperability Framework). A museum that does not offer IIIF is out of scope, even if it has a public API.

### Core feature surface (the abstraction must support these)

- **List collections** — categories / sections / folders / groups / catalogs / collections are all collapsed into one concept called *collections*.
- **List artworks in a collection, with pagination** — e.g. "artworks 200–249 of collection X". Range-style requests, not just next-page cursors.
- **Keyword search** across artworks.

### The central design tension

Museum APIs differ widely in what they support and how. Some lack search; some lack stable collection IDs; some paginate by cursor only; some expose IIIF only for images and use a separate metadata API. **The hard problem of this project is reconciling heterogeneous capabilities behind one interface** — not building any single integration. Two recurring questions to keep in mind:

1. How does the unified interface express a feature that a given museum's API cannot natively support? (Degrade? Emulate client-side? Surface as unsupported?)
2. How are pagination / search / collection semantics normalized when each museum models them differently?

When adding a new museum integration, document which features it supports natively, which are emulated, and which are unavailable.

### IIIF as the unifying primitive

Image delivery and (where available) `manifest.json` / Presentation API documents are the one thing every supported museum has in common. Prefer routing image and manifest concerns through IIIF rather than reinventing per-museum image plumbing.

## Repo state

This repository is currently empty (no commits, no source, no build system chosen). Build / lint / test commands and architectural docs should be added to this file as those decisions get made — do not invent them before then.
