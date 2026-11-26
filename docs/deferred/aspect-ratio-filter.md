# Aspect-ratio filter for museum channels

**Status:** Built in p3a 2.0 as designed here. In 1.x: deferred, design closed, no code written.
**Scope:** Hide museum artworks too elongated to display well on the
square 720x720 panel.

Moved here from `docs/art-institutions/finalized-design.md` (formerly
its §16). References to §10.2 point at that file.

The design is ready for implementation and recorded so the decisions
and the traps survive. Nothing described here exists in the firmware.

Artworks with an extreme aspect ratio (a 4:1 banner, a 1:6 scroll) are
letterboxed correctly onto the square 720×720 panel and still display
badly — mostly empty screen. The filter lets the user set a maximum
ratio past which a museum artwork is simply not shown.

## 1. Why not filter on museum metadata

The obvious design — read `entry->width`/`height` at pick time — fails
on its own terms: the
[finalized-design.md §10.2](../art-institutions/finalized-design.md)
survey found that eight of the nine museums leave those fields at 0, and the one museum whose listing dims are an
approximation rather than a description (Mia) is precisely the kind of
source a filter should not silently trust. Teaching six parsers to fill
the fields would cover six museums and leave three fail-open forever.

The dimensions the renderer actually uses are already computed, exactly
once, in the load path. Filtering there needs no parser, no extra
request, and no trust in any museum's metadata.

## 2. Architecture: measure in the loader, memoize in RAM, filter in the picker

1. **Measure.** `load_animation_into_buffer()` checks immediately after
   `loader_service_load()` returns, reading
   `loaded.info.canvas_width`/`canvas_height` — the same pair
   `build_upscale_maps_for_buffer()` consumes, so there is no second
   opinion that can disagree with the first. The check sits *before*
   `init_animation_decoder_for_buffer()`, so a rejected artwork never
   allocates `native_frame_b1` / `b2`.
2. **Skip, don't fail.** On rejection the loader reuses the existing
   blocklist-skip shape (`clear_pending_swap_state()` then
   `event_bus_emit_simple(P3A_EVENT_SWAP_NEXT)`), sharing the existing
   `MAX_BLOCKLIST_SKIPS` (6) budget. See §4 below for why the failure path
   must not be reused.
3. **Memoize.** The loader reports the measurement back via a new
   `ps_report_institution_dims(post_id, w, h)`, which walks all active
   channels and fills `width`/`height` on every matching institution
   entry — mirroring `animation_loader_evict_from_lai()`, which already
   solves the same-artwork-in-two-channels case. It deliberately does
   **not** set `cache->dirty`: the memo is RAM-only and is recomputed
   after a reboot, so no cache-format change and no carve-out in
   `art_institution_merge_entries()` is needed.
4. **Filter.** `play_scheduler_pick.c` rejects any institution entry
   whose stored dims exceed the threshold. Pure integer comparison,
   zero I/O.

`entry->width`/`height` therefore means "best known dimensions from any
source": CMA's refresh parser fills them (verified pixel-exact in
the design doc's §10.2), the loader fills them for everyone else.

**Cost.** Each *distinct* elongated artwork costs one full decode per
boot, then is filtered at pick and never loaded again. Artworks that
pass the filter cost nothing extra, ever. CMA costs nothing at all.
`jpeg_decoder_init()` fully decodes (HW `jpeg_decoder_process`, or the
SW fallback), so that one decode is not free — it is simply bounded.

**Accepted defect.** `ps_history_push()` and `last_played_id` are
committed when a swap is *queued* (`prepare_and_request_swap()` returns
`ESP_OK`), not when it displays. A rejected artwork therefore enters
history once per boot without ever appearing, so Previous can land on
an artwork the user never saw. Accepted rather than moving the commit
point, which would touch a core path well beyond this feature.

## 3. Behaviour

| Decision | Value |
|---|---|
| Scope | Museum (`PS_CHANNEL_TYPE_INSTITUTION`) channels only |
| Exempt | Pinned (`PS_CHANNEL_TYPE_PINNED`) and single-artwork push (`PS_CHANNEL_TYPE_ARTWORK`, show_url) — both explicit user intent. Manual Next is **not** exempt. |
| Unknown dims | Fail open — `width == 0 \|\| height == 0` shows the artwork |
| Comparison | `ratio > max` rejects; `ratio == max` passes |
| Range | 2.5 – 10.0, step 0.1, default **4.0**, filter **ON** by default |
| Downloads | Not gated. Elongated art still downloads and caches, so raising the threshold takes effect immediately with no refetch. |
| Empty channel | No safety valve. If everything is filtered the channel reports exhausted through the existing giveup path. |
| Browse modal | Deliberately unchanged — the playset editor still previews artworks that will not play. |

Exemptions are free: the loader already receives `channel_type`, and
pinned and single-artwork arrive under their own types.

Rotation is irrelevant: the panel is square, so a 4:1 artwork is
equally poor at any rotation.

**Settings.** `config_store` is a single JSON blob, not raw NVS keys,
so this is two fields alongside `ai_refresh_sec` / `ai_cache_size`:

```json
{ "ai_ar_filter": true, "ai_max_ar_tenths": 40 }
```

Integer tenths (40 = 4.0) keeps the comparison exact integer maths
(`w * 10 > max_tenths * h`) and matches every other setting being an
integer. The web UI renders `value / 10` with one decimal. Both the UI
and the setter clamp to the 2.5–10.0 range. The settings hint states
that the filter applies to museum channels only and never to pinned
artworks or artworks opened directly — neither is guessable from the
control itself.

Changing the threshold needs no cache invalidation: the memo stores
dimensions, not verdicts.

## 4. Implementation traps

These are the ways this feature goes wrong quietly. All were found by
reading the load path, not by testing.

- **The rejection needs its own sentinel `esp_err_t`.**
  `ESP_ERR_INVALID_SIZE` is already returned by `loader_service` for
  oversized and truncated files, which are genuine corruption and must
  still reach `animation_loader_try_delete_corrupt_cached_file()`.
  Reusing it makes the two indistinguishable. The project has no custom
  error base yet, so one needs defining.
- **Never route the rejection through the failure path.** Three
  landmines in `animation_player_loader.c`:
  1. `animation_loader_try_delete_corrupt_cached_file()` fires for any
     failing path containing `/museum/`. An elongated artwork would be
     deleted from the vault and evicted from LAi, then re-downloaded on
     the next refresh, and deleted again — a permanent bandwidth loop
     that also reads as corruption in the logs.
  2. `MAX_AUTO_RETRIES = 3` — three elongated picks in a row would
     paint "Playback Error" for what is a deliberate policy decision.
  3. `proc_notif_fail_if_processing()` would report failure on the
     processing indicator.
- `SWAP_FAIL_LOUD` is set only by `execute_playset()` on a
  user-initiated playset switch, not by a manual Next press — so the
  on-screen-error risk is narrower than it first appears, but it is not
  zero.
- Rejection chains are self-terminating: every rejection memoizes, so
  the worst case is that the loader measures a channel once and the
  picker then reports exhausted. The `MAX_BLOCKLIST_SKIPS` bound exists
  to cap the *burst* of decodes, not to prevent a loop.
- Per-swap pick logging stays at INFO (operator-facing), consistent
  with the existing `ps_chsel` / `ps_pick` lines.

## 5. Verification

Set the threshold to its 2.5 floor on-device: ordinary landscape works
past 5:2 then trip the filter, so the path is exercised without needing
a curated set of extreme artworks. Confirm that (a) the rejected
artwork is skipped silently with no error overlay, (b) it is not
deleted from the vault, (c) a second encounter with the same artwork
costs no decode, and (d) pinning that artwork makes it play.
