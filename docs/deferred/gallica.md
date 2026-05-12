# Gallica (BnF) integration

**Status:** Deferred (M6 — 2026-05-12).
**Scope:** Adding BnF Gallica as a sixth museum channel.

## What was deferred

Implementing a Gallica adapter alongside the existing five museums
(AIC, Rijksmuseum, V&A, Wellcome Collection, SMK).

## Why

Gallica's catalogue API is SRU (Search/Retrieve via URL) and returns
XML — specifically, an OAI-PMH-flavored Dublin Core record schema
wrapped in an SRU response envelope. The rest of the museum surface is
JSON-only; the codebase does not include an XML parser and ESP-IDF
does not ship one. Adding Gallica would require either:

1. Bundling a lightweight XML parser (mini-xml, ezXML, expat). All add
   meaningful binary-size and RAM cost; each needs its own porting and
   review effort to confirm it builds clean against ESP-IDF v5.5.x.
2. Hand-rolling an SRU-specific parser in plain C that extracts the
   fields we need (`numberOfRecords`, `record/recordData`, `creator`,
   `title`, `identifier`, `type`, `date`) from a tag stream.

Either path is multiple days of work that the existing five museums
already cover in image-content terms.

There's also a User-Agent quirk: Gallica returns HTTP 403 to default
`requests` / `curl` fingerprints, so the device would have to send a
browser-like UA string — manageable but worth noting.

## Revisit when

- A lightweight XML parser becomes available in ESP-IDF, or
- Field usage shows a content-diversity gap that Gallica would uniquely
  fill (BnF's manuscripts, maps, sheet-music coverage is strong and not
  duplicated elsewhere in the museum set), or
- A user volume justifies the integration cost.

## Reference materials kept under

`reference/museum-art/source/gallica/` — run.py, report.md, sample
images. The reference run validates the API shape (SRU pagination,
IIIF v1.1 download at 720px longest side) so a future implementer
won't need to rediscover the basics.
