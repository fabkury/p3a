# LoC `iiif_key` width — drop > 47-char entries

**Status:** Deferred (M-LoC — 2026-05-13).
**Scope:** Library of Congress items whose IIIF identifier is 48 chars
or longer.

## What was deferred

`components/art_institution/museums/loc.c` silently drops any listing
entry whose extracted IIIF identifier is 48 chars or longer. Browser-side
preview population in `webui/museum/loc.js` applies the same filter so
the user never sees a "Next →" land on an item the device cannot
actually store.

The dropped items aren't a small tail. Three concrete categories from
the investigation:

- **Newspaper batches** (`service:ndnp:...`) — 60–100 chars. Excluded
  from the v1 LoC channel by format-axis choice anyway, but if a
  future axis exposes newspaper-bearing facets, every entry would be
  dropped.
- **Music silent-film records** (`service:music:mussilentfilms:...`)
  — 120+ chars. 86 % of the `notated music` facet yields IIIF in
  listings but only 2.3 % fit the 48-char slot.
- **Audio-folklore archives** (`service:afc:afc2019048:...`) — 80+
  chars. Sprinkled through the `manuscript/mixed material` facet that
  v1 does include; the long-form entries are dropped, the short ones
  are kept.

## Why

The `institution_channel_entry_t.iiif_key` field is `char[48]`. The
struct is 64 bytes packed and shares the channel-cache slot with
`makapix_channel_entry_t` and `giphy_channel_entry_t`, both also
64 bytes. The cache record uses `entry->extension` as a one-byte
file-format tag with 0xFE / 0xFF as reserved sentinels (used by Rijks's
Linked-Art walk — see
[`docs/art-institutions/finalized-design.md`](../art-institutions/finalized-design.md)
§4.2 and §15.4). Every byte of the 64-byte slot is currently spoken
for.

Widening `iiif_key` therefore means:

1. **Choose a new entry width.** The longest observed working IIIF id
   is 125 chars (`service:music:mussilentfilms:...`). A 128-byte
   `iiif_key` would cover the long tail; 96 bytes covers most music
   archives but not silent-film records.
2. **Bump the channel cache record size** for the entire institution
   record type, **or** introduce a discriminated alternative layout
   (e.g. `institution_channel_entry_v2_t`). Either path is a binary
   format break.
3. **Update the channel-cache loader/saver** in
   `components/channel_manager/channel_cache.c` to recognize and
   migrate the existing 64-byte entries. Field offsets shift in the
   wider layout, so all reads need rewriting.
4. **Update the LAi bitmap sizing** if it tracks per-entry width.
5. **Update the playset binary format** if the entry size affects
   `cache_size` accounting (the `CHANNEL_CACHE_HARD_CAP = 4096`
   bound is in entries, not bytes — that part is unaffected).

The change touches `channel_cache.c`, `channel_cache_evict.c`,
`art_institution_types.h`, every museum adapter that reads/writes
`institution_channel_entry_t`, plus a backward-compat loader for the
64-byte form. Not small.

## Estimated impact

For the **format facets v1 ships with** (`photo, print, drawing`,
`manuscript/mixed material`, `3d object`), the drop rate inside the
IIIF-bearing subset is:

| Facet | IIIF-bearing | fit_48 of IIIF-bearing |
|---|---:|---:|
| photo, print, drawing | 8 % | 75 % |
| manuscript/mixed material | 28 % | 32 % |
| 3d object | unverified | unverified |

So today the cap silently drops 25–68 % of would-be-usable items
inside each LoC channel. That isn't trivial; it's just much cheaper
to accept the loss than to widen the entry.

## Revisit when

Any of these would justify the binary format break:

- Field usage shows users wanting newspaper, music, or audio-folklore
  content delivered as image strips, and the v1 channels feel
  starved.
- A second museum needs longer IIIF identifiers and the change
  amortizes across two integrations.
- The channel cache layout is being changed for an unrelated reason
  (e.g. adding a new field for some other museum) — in which case
  bundling the `iiif_key` widening into the same format bump is
  almost free.

Field measurement is the trigger: if all three v1 LoC channels run
healthily without users asking for the long-id formats, the cost
isn't justified. Re-evaluate after a few months of field telemetry on
LoC channel completeness and download success rates.
