# Wellcome long-label terms

**Status:** Deferred (M4 — 2026-05-12).
**Scope:** Wellcome facet terms whose label is longer than 32 characters.

## What was deferred

Wellcome facet terms whose `label.length > 32` are hidden from the
museum browse modal. This applies to all four Wellcome axes
(`workType`, `genres`, `subjects`, `contributors`), but only matters
for the latter three: `workType` exposes a stable short id (`k`, `q`,
…) regardless of label length, and its labels are short anyway.

## Why

The playset binary format's channel `identifier` field is `char[33]`
— 32 characters plus a null terminator. For non-`workType` Wellcome
axes the catalogue API does not expose a stable short id; the term
label IS the filter value used on `genres.label=`, `subjects.label=`,
or `contributors.agent.label=`. Storing the term in the playset
identifier therefore caps the label at 32 characters.

The cheapest correct behavior is to hide longer-than-32-char terms
from the browse modal. The user never sees them, the device never
stores them. No silent truncation, no risk of two long labels sharing
a 32-char prefix colliding.

## Estimated impact

Small but non-zero. Most Wellcome subject and genre labels are well
under 32 chars (e.g. `Botany`, `Engraving and Engravings`,
`Portrait prints`). The hide rule mostly affects long-tail
contributor labels with affiliation strings appended (e.g.
`David Gregory & Debbie Marshall` — 31 chars, fits; some institutional
contributors with longer names do not).

## Revisit when

Field usage shows enough valuable Wellcome terms hidden to justify
expanding the playset `identifier` slot. The change touches:

- `components/play_scheduler/include/play_scheduler_types.h` —
  two `identifier[33]` members.
- `components/play_scheduler/include/playset_store.h` — the on-disk
  binary format (P3PS magic, would require v11 → v12).
- `components/play_scheduler/playset_store.c` — the backward-compat
  loader for v11 playsets.
- `components/play_scheduler/playset_json.c` — JSON serializer.
- `components/play_scheduler/play_scheduler.c` —
  `ps_compute_channel_id()` hash input width.

Not a small change; defer until measurement justifies it.
