# Channel refresh race: Ci updated before LAi rebuild

**Status:** Open. Observed once so far. No user-visible impact (picker falls
back to other channels), but a real ordering bug worth fixing.
**First observed:** 2026-05-05.
**Severity:** Low. Two warning log lines per occurrence; one or two picks
silently retry on a different channel.

---

## TL;DR

During a periodic channel refresh, there is a brief window in which the
channel's content index `Ci` has been updated to reflect newly fetched
entries, but the corresponding "locally available" index `LAi` has not yet
been rebuilt. If the play scheduler picks during this window, it sees
`Ci > 0, LAi = 0` and logs a `RandomPick ... FAIL` warning. The picker
gracefully falls back to another channel, so playback continues, but the
warning is real and indicates a sequencing issue in the refresh path.

---

## What was observed

From a Giphy Trending refresh on 2026-05-05 (timestamps in ms since boot):

```
I (3633719) giphy_refresh: Full refresh: evicted 1 orphaned entries, 255 kept
I (3635250) ps_pick: TOTALS: active_channels=3, total_Ci=1953, total_LAi=1698
I (3635250) ps_chsel: Stochastic selected channel[0] 'Giphy Trending' (credit was 90696, now 25160)
W (3635254) ps_pick: RandomPick Makapix[0] 'b3770b92bcf5bbc4': FAIL - Ci=255 but LAi=0 (no downloaded files)
W (3635264) ps_pick: Channel[0] exhausted, trying next
I (3635288) ps_chsel: Stochastic selected channel[0] 'Giphy Trending' (credit was 55407, now -10129)
W (3635289) ps_pick: RandomPick Makapix[0] 'b3770b92bcf5bbc4': FAIL - Ci=255 but LAi=0 (no downloaded files)
W (3635299) ps_pick: Channel[0] exhausted, trying next
I (3635304) ps_chsel: Stochastic selected channel[3] 'Fab' (credit was 9998, now -55538)
I (3635330) ps_pick: >>> PICKED (RandomPick): LAi_index=669, Ci_index=669, post_id=947, pool_size=1026, attempt=1, storage_key=b4c9dacf...
I (3635518) giphy_refresh: LAi rebuilt after refresh: 255 files available
I (3635859) giphy_refresh: Giphy channel 'Giphy Trending' refresh complete: 255 fetched, 255 in cache
```

Race window:

- **t=3633719** — refresh evicts 1 orphan, leaves 255 entries → Ci is now 255.
- **t=3635250** — picker selects Giphy Trending; `total_Ci=1953` but
  `total_LAi=1698` (the channel's individual LAi is 0 at this moment).
- **t=3635254** — pick fails: `Ci=255 but LAi=0`.
- **t=3635288** — picker tries again, fails again same way.
- **t=3635304** — falls back to channel 'Fab', picks successfully.
- **t=3635518** — `LAi rebuilt after refresh: 255 files available` — the
  race window closes ~268 ms after Ci was updated.

Note also `active_channels=3` in that TOTALS line — Trending appears to be
temporarily marked inactive/under-rebuild during this window, but the
channel selector still considered it (credit was high at 90696). The exact
relationship between the active-count and the picker's eligibility list
hasn't been investigated.

## Diagnosis (preliminary)

The ordering during a Giphy refresh appears to be:

1. Fetch all pages from Giphy API.
2. Apply eviction → update channel's `Ci` (content index).
3. *Resume normal scheduling* — picker can run here.
4. Rebuild `LAi` (locally-available index, mapping Ci entries to downloaded
   files on the SD card vault).
5. Mark refresh complete.

Step 3 and step 4 should be atomic from the picker's point of view, or step
3 should not happen until after step 4 completes. The window is short
(~hundreds of ms in the observed case), but on a busy device it's clearly
hittable.

This is a guess from log evidence, not from reading the code. Confirm by
inspecting `components/giphy/giphy_refresh.c` and the LAi rebuild in
`components/play_scheduler/ps_lai.c` (or wherever LAi is rebuilt) before
implementing a fix.

## User impact

- None observed. The picker logs `Channel[0] exhausted, trying next` and
  falls back to another channel; playback continues without interruption.
- If *every* channel were refreshing at the same instant the picker ran,
  there could be a moment where no channel is pickable. With four channels
  and per-channel refresh windows, this hasn't been observed.

## Why fix it anyway

- The warning lines are noise during normal operation, making it harder to
  spot real picker failures in logs.
- The race may have edge cases under heavier load (e.g. a slower LAi rebuild
  could miss many more picks).
- Hints at a broader sequencing assumption that may be incorrect in other
  refresh paths.

## Possible fixes (not yet decided)

- **Hold the picker out of the channel until LAi is rebuilt.** Either gate
  picks on a per-channel `refresh_in_progress` flag, or have LAi-rebuild
  happen *before* the public Ci is swapped in (build new LAi against new Ci
  in a staging area, then atomically swap both pointers).
- **Reverse the order:** rebuild LAi first, then publish Ci.
- **Suppress the warning** during a known refresh window — papers over the
  race, doesn't fix it. Not recommended.

## References

- Log evidence: monitor capture from 2026-05-05.
- Related (different bug, same firmware run): `docs/sdio-rx-oom-crash.md`
  Occurrence 4 — the same monitor capture also shows a near-miss of the
  SDIO RX OOM crash later in the run.
- Code to inspect when picking this back up:
  - `components/play_scheduler/play_scheduler_refresh.c` — refresh
    orchestration; check ordering of Ci publish vs LAi rebuild
  - `components/play_scheduler/play_scheduler_lai.c` — LAi build
  - `components/play_scheduler/play_scheduler_pick.c` — picker; source of
    the `RandomPick ... FAIL - Ci=N but LAi=0` warning
  - `components/giphy/giphy_refresh.c` — Giphy-specific refresh path that
    invoked the `Full refresh: evicted ... orphaned entries` log line
