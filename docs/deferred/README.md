# Deferred design decisions

Decisions we considered, intentionally didn't ship, and want to revisit.
Each entry names: what was deferred, why now isn't the right time, what
would change that.

- [Gallica integration](gallica.md) — XML/SRU parser dependency.
- [Wellcome long-label terms](wellcome-long-labels.md) — terms whose
  label exceeds the playset 32-char identifier limit.
- [LoC `iiif_key` width](loc-iiif-key-48-char.md) — Library of Congress
  items whose IIIF identifier exceeds the 47-char cache-entry slot.
