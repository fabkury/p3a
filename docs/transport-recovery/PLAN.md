# ESP-Hosted SDIO transport-failure recovery

**Live guide.** This file tracks the phased work. Update the status column and
the log at the bottom as work lands; record decisions here, not in chat
history.

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Orderly reboot instead of esp_hosted's instant restart | **Done** — shipped + validated on-device (2.7.0 bench unit); 2 quick checks (normal-boot log, streak-reset-on-GOT_IP) + passive soak open (see checklist) |
| 1 | In-place transport recovery (no reboot), version-gated | **Ready to start** — needs a slave-2.9.3 bench unit; see the Phase 1 guide |
| 2 | Bundle slave fw 2.9.7 (contingent on Phase 1 bench findings) | Not started |

---

## Handoff — resuming from another workstation

State as of 2026-06-06 (handoff written for resuming on the workstation with
the **slave-2.9.3 bench unit**; all work so far was done and tested against
the **2.7.0 bench unit**). Identify a unit's slave version from the boot log:
`slave_ota: Current co-processor firmware: X.Y.Z`.

**Commits so far** (all 2026-06-06, branch `main`):

- `ace926cb` — chore: sdkconfig/dependencies.lock regenerated under ESP-IDF
  **5.5.1** (this workstation built with 5.5.1). If the next workstation uses
  5.5.2, sdkconfig will churn again on first build — commit that churn as its
  own `chore:` commit per repo precedent (`272485a0`, `d9c5b393`, `ace926cb`)
  and keep feature diffs clean.
- `370db897` — feat: the entire Phase 0 implementation + the temporary test
  hook + this plan. Read this commit to see every Phase 0 touch point.
- `d040e58f` — docs: degraded-mode quiesce validation logged.

**Decisions already made (do not relitigate without new evidence):**

1. Containment + recovery on our side; upgrading esp-hosted does not fix the
   underlying bug (open upstream, reproduced through 2.12.x).
2. Version-gate enhancements, never the safety ladder (see Design rule).
3. The test hook **stays until Phase 1 is validated** — `?real=1` is the
   injection rig Phase 1 needs. It is Kconfig-gated (default n), so
   release-from-defaults stays safe meanwhile.
4. Heartbeat monitoring is Phase 2 material: it needs slave >= 2.9.4 and the
   fleet maxes out at 2.9.3 until a slave bump ships.
5. CP_INIT stays log-only in Phase 0; Phase 1 may act on it once trusted.
6. No `transport_degraded` field in `/status`: when degraded, the device
   has no connectivity at all, so nothing could ever read it. The NVS
   counters (read after recovery/reboot) are the fleet telemetry.

**Immediate next actions, in order:**

1. Quick Phase 0 checks on any unit (~15 min, see checklist): normal-boot
   log line; streak-reset-on-GOT_IP. *(plain `?real=1` reboot variant +
   `transport_recovery_reboots` in `/status` — done 2026-06-08.)*
2. Start/continue the passive soak (64-channel Giphy playset running; wait
   for the organic failure; confirm orderly path). Do not block on this.
3. Phase 1 development + validation on the 2.9.3 unit (guide below).
4. Regression-check the gate on a 2.7.0 unit (must take the Phase 0 path).
5. Delete the test hook (five files; `grep -rn "TRANSPORT_FAULT_INJECT"`),
   final commit, mark Phase 1 done here.

---

## Background

### Failure signature (for future log greps)

```
E H_SDIO_DRV: Dropping packet(s) from stream
E H_SDIO_DRV: Failed to push data to rx queue
E sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x109   <- ESP_ERR_INVALID_CRC
E H_SDIO_DRV: sdio_write_task: 0: Failed to send data: 265 66 66
E sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107   <- ESP_ERR_TIMEOUT
E H_SDIO_DRV: sdio_write_task: 1: Failed to send data: 263 66 66
E H_SDIO_DRV: Unrecoverable host sdio state
I os_wrapper_esp: Restarting host                                  <- instant esp_restart()
```

Observed 2026-06-06 at ~2.3 h uptime during sustained 64-channel Giphy
refresh + concurrent GIF downloads (full-duplex SDIO load). Recurring.

### Mechanism

The SDIO RX stream loses framing (`is_valid_sdio_rx_packet` fails mid-stream,
`sdio_drv.c:922`), then TX CMD53 writes fail with CRC then timeout.
`MAX_SDIO_WRITE_RETRY` is 2 (`sdio_drv.c:119`); after that the driver declares
the transport dead. All four `_h_restart_host()` call sites in `sdio_drv.c`
(lines ~509, ~694, ~1095, ~1115) are gated by
`CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE`, and each posts
`ESP_HOSTED_EVENT_TRANSPORT_FAILURE` *regardless* of that setting — that
guarantee is what this plan builds on.

The combined evidence (RX desync + TX CRC at the same moment) means the C6
wedged or the bus glitched under sustained load. Possible contributing factor
worth keeping in mind: the C6 (SDMMC slot 1) and the microSD share controller
resources, and failures correlate with heavy simultaneous vault writes +
Wi-Fi traffic.

### Upstream status

Known, **open** esp-hosted-mcu bug; reproduced from v2.3.2 through v2.11.6 and
v2.12.x on multiple P4+C6 boards (incl. Waveshare), not fixed by clock
reduction to 20 MHz:

- https://github.com/espressif/esp-hosted-mcu/issues/121 (iperf, 2.3.2)
- https://github.com/espressif/esp-hosted-mcu/issues/167 (2.11.6, multiple vendors)
- https://github.com/espressif/esp-hosted-mcu/issues/184 (inbound TCP stalls; RX **streaming mode** implicated)

Conclusion: upgrading esp-hosted does not fix this. Production strategy is
containment + recovery on our side.

### Sibling issue

`docs/sdio-rx-oom-crash.md` tracks a *different* failure in the same subsystem
under the same trigger load: RX buffer alloc failure -> unconditional `assert`
-> panic. That one is a panic (cannot be intercepted); its tabled fix
(switch SDIO RX mode streaming -> `MAX_SIZE`) would *also* remove the
streaming-mode framing-loss path involved here (issue #184). If that fix
lands, both problems shrink. This plan handles whatever still gets through.

### Version landscape

| Piece | Version | Notes |
|---|---|---|
| Host esp_hosted lib | 2.9.7 (pinned `~2.9.3`) | 2.10+ blocked by internal-RAM mempool growth (see `main/idf_component.yml`) |
| Bundled slave fw | 2.9.3 (`slave_ota.c`) | upgrade-only gate; flashes when C6 reports older |
| Stuck fleet devices | 2.7.0 | OTA bug in 2.7.0 slave blocks ANY upgrade ([#143](https://github.com/espressif/esp-hosted-mcu/issues/143)); `slave_ota.c` LOCKED_VERSION gate skips them |
| Relevant upstream fixes | 2.9.4: events (CP_INIT w/ reset reason, HEARTBEAT, TRANSPORT_FAILURE) + "fixed issues that prevent transport reinitialisation"; 2.9.7: deinit->init leak fixes; 2.11.0: RX-failure double-free fix; 2.12.8: `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` | |

---

## Constraints

1. **Slave-2.7.0 fleet devices must not regress.** They can never be updated
   (#143). Anything we ship must work on them or degrade to "no worse than a
   reboot".
2. **slave_ota is upgrade-only** (`slave_ota.c` version gate): a bundled slave
   fw bump cannot be rolled back OTA. Bench-validate before shipping any bump.
3. **Internal RAM is tight** (reason for the 2.9.x pin). Recovery machinery
   must not add meaningful permanent internal-RAM cost.

## Design rule

**Version-gate the enhancements; never version-gate the safety ladder.**

Every device, regardless of slave version, always has a path that terminates
in: orderly reboot -> reboot-streak guard -> degraded (playback-only) mode.
Enhancements (in-place recovery, heartbeat, reset-reason telemetry) are
enabled per slave version on top of that invariant.

Why the ladder is slave-version-agnostic: `ESP_HOSTED_EVENT_TRANSPORT_FAILURE`
is generated host-side with zero slave participation. Why Phase 1 is expected
to be version-agnostic too: in-place recovery hard-resets the C6 via GPIO 54
and re-runs the standard SDIO enumeration + `ESPInit` handshake — from the
C6's perspective indistinguishable from a host reboot, which every slave
version (incl. 2.7.0) already survives on every boot
(`CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y`). Still gated +
bench-validated per the rule above.

---

## Phase 0 — orderly reboot

**Goal:** stop esp_hosted from calling `esp_restart()` behind our back; take
ownership of the failure with telemetry, an on-screen notice, a short quiesce
window, and a loop guard. Identical behavior for all slave versions; pure
improvement on every axis.

### Behavior spec

On `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` (first occurrence only; later
duplicates ignored):

1. Increment `transport_reboot_total` (NVS).
2. If `transport_reboot_streak >= 3`: do **not** reboot. Log + transient
   on-screen notice (~5 s, then playback resumes the screen); stay in
   degraded playback-only mode (a device whose transport dies on every boot
   must not reboot-loop). Degraded mode **quiesces the network stack** so
   nothing keeps feeding the dead SDIO link (else: endless CMD53 retry/log
   storms, MQTT reconnect loops, and futile Wi-Fi reinit cycles full of RPC
   timeouts — observed in the 2026-06-06 real-injection test):
   - STA netif brought down directly (`esp_netif_action_disconnected`) —
     lwIP stops routing, sockets fail fast locally, driver TX storm ends;
   - the standard disconnect signals are fired (`makapix` signals +
     `P3A_EVENT_WIFI_DISCONNECTED` + MQTT stop) — the same calls app_wifi's
     STA_DISCONNECTED handler would make, which can never fire by itself
     because the disconnect event would have to arrive over the dead
     transport;
   - the Wi-Fi health monitor stands down (polls
     `transport_recovery_is_degraded()`), so it cannot start futile
     full-reinit cycles or trigger its own hard reboot.
   Degraded is terminal until reboot/power-cycle.
3. Else: increment streak, then from a dedicated task: 5 s on-screen countdown
   ("WiFi chip connection lost / Restarting in N...") giving in-flight network
   error paths time to unwind file handles, then `esp_restart()`.
4. Streak resets on `IP_EVENT_STA_GOT_IP` (same lifecycle as the existing
   `wifi_reboot_streak`).

On `ESP_HOSTED_EVENT_CP_INIT`: log only. First one per boot is the normal C6
boot announcement; later ones are logged loudly as "C6 rebooted underneath
us" with the co-processor reset reason (field is valid on >= 2.9.x slaves;
reads 0/unknown on 2.7.0). **No action in Phase 0** — zero false-positive
risk; gathers fleet evidence for Phase 1.

### Changes

| File | Change |
|---|---|
| `sdkconfig` | `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE` =y -> not set |
| `components/wifi_manager/transport_recovery.c` | **new** — event handler + orderly-reboot task + streak guard |
| `components/wifi_manager/wifi_manager_internal.h` | declare `transport_recovery_register_events_once()` |
| `components/wifi_manager/app_wifi.c` | register handler at top of `app_wifi_init()` (before any `esp_wifi_remote_init` can bring the transport up — covers both STA and captive-portal paths); reset transport streak on GOT_IP |
| `components/wifi_manager/wifi_recovery.c` | drive-by fix: pre-existing hard-reboot countdown was invisible during playback for the same reason (no `display_renderer_enter_ui_mode()`); now uses the same UI-mode switch |
| `components/wifi_manager/CMakeLists.txt` | add source + `esp_hosted` dep |
| `components/config_store/config_store.h` / `config_store_giphy.c` | `transport_reboot_total` / `transport_reboot_streak` counters (NVS keys `tp_rst_tot` / `tp_rst_str`), mirroring the wifi/touch reboot counters |
| `components/http_api/http_api_rest_status.c` | expose `transport_recovery_reboots` in `/status` (only when > 0) |
| `components/ota_manager/ota_manager_install.c` | reset transport counters on OTA validation (parity with wifi/touch) |

### Deliberately omitted (with rationale)

- **`channel_cache_flush_all()` before reboot** — avoids a wifi_manager ->
  channel_manager dependency (cycle risk) and a hazard if USB-MSC owns the SD
  card at failure time. The save debounce's 240 s hard ceiling already bounds
  staleness; the countdown's real value is letting error paths close files.
- **Acting on unexpected CP_INIT** — false-positive reboot risk on a healthy
  device (exactly the regression the 2.7.0 constraint forbids). Log-only
  until Phase 1.
- **Heartbeat** — needs slave >= 2.9.4, and the fleet maxes out at 2.9.3
  until a slave bump ships; Phase 2 material.
- **ugfx diagnostics screen row** — the "Reboots" row in `main/ugfx_ui.c`
  formats wifi+touch combinatorially; adding a third counter there is a
  cosmetic refactor. `/status` + logs are the telemetry channels for now.

### Validation checklist

- [x] Build passes (user builds; not part of this change).
- [ ] Normal boot: "C6 co-processor up" logged once; no behavior change.
- [x] Fault injection — realistic (no physical probe needed; automated via
      the hook): `curl -X POST "http://p3a.local/action/inject_transport_failure?real=1"`
      while downloads are active; the host driver hits real CMD53 failures ->
      TRANSPORT_FAILURE -> countdown UI -> reboot -> reconnect; `/status`
      shows `transport_recovery_reboots` incremented.
      *(2026-06-06: detection chain validated via `?real=1&streak=3` on the
      2.7.0 bench unit — pulse -> CMD53 errors -> TRANSPORT_FAILURE -> handler
      in ~50 ms, degraded branch + notice OK. 2026-06-08: plain `?real=1`
      reboot variant on a clean streak validated end-to-end on the 2.7.0 unit
      — GPIO-54 pulse -> first CMD53 0x107 at +5 ms -> handler at +62 ms ->
      "Entering UI mode" -> 5..1 countdown -> clean SW_CPU_RESET -> reconnect;
      `/status` then reported `transport_recovery_reboots:1`.)*
- [x] Fault injection — synthetic (no hardware poke), via the temporary hook
      (see below): `curl -X POST "http://p3a.local/action/inject_transport_failure"`
      -> countdown UI -> reboot -> `transport_recovery_reboots` increments.
      *(2026-06-06: reboot + on-screen countdown confirmed; counter value in
      `/status` not yet explicitly checked.)*
- [x] Degraded mode (one shot):
      `curl -X POST "http://p3a.local/action/inject_transport_failure?streak=3"`
      -> no reboot, "Playback continues offline" notice, playback keeps
      running. *(2026-06-06: confirmed incl. on-screen notice.)*
- [ ] Streak resets after a successful connection (GOT_IP): after the
      degraded test, reboot manually, let Wi-Fi connect, then a plain
      injection must take the countdown-reboot path again.
- [ ] Soak under the trigger load (64-channel Giphy playset) until a real
      failure occurs; confirm orderly path end-to-end.
- [ ] ~~Delete the temporary test hook once the above pass~~ — **deferred:**
      the hook is Phase 1's test rig (decision 2026-06-06); deletion is the
      last item of the Phase 1 checklist.

### Temporary test hook (TRANSPORT_FAULT_INJECT) — delete after Phase 1

Gated by `CONFIG_P3A_TRANSPORT_FAULT_INJECT` (currently **=y** in sdkconfig
for the validation builds; default n, so a release built from defaults would
exclude it — but delete it outright once Phase 1 validation is done).

Usage:

```
POST /action/inject_transport_failure            -> synthetic: handler only; 5 s countdown, reboot
POST /action/inject_transport_failure?streak=3   -> synthetic: degraded-mode path, no reboot
POST /action/inject_transport_failure?real=1     -> REALISTIC: pulses the C6 reset GPIO (54);
                                                    the SDIO driver itself detects the dead
                                                    slave (CMD53 errors -> retries ->
                                                    "Unrecoverable host sdio state" ->
                                                    TRANSPORT_FAILURE) — full production chain
```

`?streak=N` presets the persisted streak counter first (works in both modes;
`?real=1&streak=3` tests the degraded branch via real detection).
Repeated injections in one boot each act fresh (the hook un-latches the
handler; the real failure path latches on first occurrence).

Note: the degraded branch **quiesces the network stack** (netif down, MQTT
stopped, health monitor parked — terminal until reboot). With synthetic
`?streak=3` the transport is actually alive, but the device still goes
offline by design — reboot it to restore connectivity after that test.

Real-mode notes: the pulse replicates the driver's own reset sequence
(10 ms hold, same polarity from `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH`).
Detection fires on the next host->C6 SDIO write, so run it while the device
has traffic (downloads active, or just MQTT keepalive / health-check DNS —
expect up to a few seconds of delay when idle).

**To delete every piece:** `grep -rn "TRANSPORT_FAULT_INJECT"` — five files:

1. `components/http_api/http_api.c` — handler block + router branch
2. `components/wifi_manager/transport_recovery.c` — inject function at bottom
3. `components/wifi_manager/CMakeLists.txt` — the `esp_driver_gpio` dep line
4. `main/Kconfig.projbuild` — the Kconfig option
5. `sdkconfig` — the `CONFIG_P3A_TRANSPORT_FAULT_INJECT=y` line

---

## Phase 1 — in-place transport recovery (ready to start)

**Goal:** on transport failure, recover without rebooting the P4: playback
never blinks; ~8–18 s network outage (deinit + 1.5 s slave reset delay
`CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS` + card re-init + Wi-Fi/DHCP + MQTT).

**Prerequisite:** a bench unit whose C6 reports >= 2.9.3, with the test hook
still compiled in.

### Why this is expected to work on the pinned versions

- Host lib 2.9.7 contains the 2.9.4 fix "fixed ESP-Hosted and SDIO issues
  that prevent transport reinitialisation" and 2.9.7's deinit->init memory
  leak fixes.
- The vendor demonstrates this exact flow:
  `managed_components/espressif__esp_hosted/examples/host_hosted_events/`
  (README shows a full successful in-place recovery from this exact error —
  deinit -> init -> GPIO-54 slave reset -> card re-init -> reconnect).
  **Known quirk** from that README's log: reinit prints
  `W H_API: Transport already initialized, skipping initialization` — the
  deinit/init path has version-specific internal-state behavior; it works
  anyway, but treat that warning as expected, and re-validate on any
  esp_hosted upgrade.
- Open question Phase 1 answers empirically: whether the **slave 2.9.3** side
  blocks reliable reinit (the 2.9.4 changelog does not say which side those
  fixes touched; the slave-side experience of an in-place recovery is
  identical to a host reboot, so probably fine). If reinit proves unreliable
  against 2.9.3 -> Phase 2.

### API surface (all already in the pinned component)

- `esp_hosted.h`: `esp_hosted_init()`, `esp_hosted_deinit()`,
  `esp_hosted_connect_to_slave()`
- `esp_hosted_misc.h`: `esp_hosted_get_coprocessor_fwversion()`
- `esp_hosted_event.h`: events (already consumed by `transport_recovery.c`)

### Critical gotcha: cache the slave version at boot

At failure time the transport is dead — `esp_hosted_get_coprocessor_fwversion()`
is an RPC and **cannot be called then**. Cache it while the transport is up:
`slave_ota` already queries it during boot (`slave_ota.c`, the
`current_ver` it logs); either expose a getter from there or query once from
`transport_recovery.c` after the first CP_INIT. Gate on the cached value;
treat "never learned" as "old slave" (Phase 0 path).

### Implementation sketch (code anchors)

1. `transport_recovery.c`, TRANSPORT_FAILURE case: insert the gate + recovery
   dispatch *before* the existing streak/reboot logic; refactor the reboot
   branch into a callable helper — it becomes the escalation target.
2. Recovery runs in `wifi_recovery_task` (extend its notification into a
   command protocol — today it is a plain binary notify from
   `wifi_schedule_full_reinit()`). Keeps internal-RAM cost ~0. Respect/reuse
   `s_reinit_in_progress` (the health monitor force-clears it after 120 s —
   extend that guard to the transport-recovery command).
3. Recovery sequence (mirror the vendor example, then reuse our existing
   Wi-Fi machinery):
   set "recovering" flag -> netif down (`esp_netif_action_disconnected`,
   as in the degraded quiesce) -> `esp_wifi_remote_stop/deinit` best-effort
   (RPC calls will error/time out over the dead link — tolerate) ->
   `esp_hosted_deinit()` -> `esp_hosted_init()` ->
   `esp_hosted_connect_to_slave()` (hard-resets C6 via GPIO 54) -> existing
   `wifi_recovery_task` reinit flow (it already does
   esp_wifi_remote_init/set_mode/set_config/start/connect with backoff and
   credential reload) -> success = WIFI_CONNECTED_BIT within 15 s + the DNS
   health check.
4. CP_INIT **during** recovery is expected (we reset the slave) — gate the
   "C6 rebooted underneath us" warning with the recovering flag (the vendor
   example uses `resetting_esp_hosted_transport` for exactly this).
5. Bounded attempts (2–3) -> escalate to the Phase 0 orderly-reboot helper.
   The Phase 0 streak guard still terminates the whole ladder in degraded
   mode.
6. **Hang protection:** `esp_hosted_deinit()` can in principle block on a
   semaphore held by a wedged transport task. Arm a one-shot `esp_timer`
   escalation (e.g. 60 s) before starting recovery, cancel on success; if it
   fires, run the Phase 0 orderly reboot. Without this, a hung recovery would
   leave the device broken with no way out (today's instant-restart at least
   guaranteed a reset).
7. Per-cycle heap watermark logging (free internal heap + largest block)
   to confirm the 2.9.7 leak fixes hold across repeated recoveries.
8. Caller audit before enabling: grep app components for `ESP_ERROR_CHECK`
   wrapping `esp_wifi_remote_*` / `esp_wifi_*` calls that could abort when
   RPCs fail during the transport-down window; soften to logged errors.

### Policy decisions to make during implementation

- Does the streak guard gate recovery *attempts* or only *reboots*?
  (Suggestion: only reboots — in-place attempts are cheap and invisible.)
- Treat repeated unexpected CP_INIT (outside recovery) as a transport
  failure? (Suggestion: yes after Phase 1 soaks, since recovery is now
  cheap; keep log-only until then.)
- Cap on lifetime in-place recoveries before scheduling a maintenance reboot
  at an idle hour (P4 uptime becomes unbounded once reboots stop; slow leaks
  elsewhere lose their accidental "cleanup").

### Phase 1 validation checklist

- [ ] On the 2.9.3 unit, Phase 0 quick checks first (baseline: hook + reboot
      path behave exactly as on the 2.7.0 unit).
- [ ] `?real=1` under load -> in-place recovery: driver errors ->
      TRANSPORT_FAILURE -> recovery -> Wi-Fi + MQTT back, downloads resume,
      **screen never blinks**, no reboot (uptime preserved).
- [ ] Repeat >= 5 cycles; heap watermarks stable across cycles.
- [ ] Recovery-failure path: induce failure during recovery (e.g. hold the
      C6 in reset by re-pulsing mid-recovery) -> bounded attempts ->
      orderly reboot escalation works.
- [ ] Unexpected-CP_INIT logging still correct (no false "rebooted underneath
      us" during recovery).
- [ ] Regression on the 2.7.0 unit: `?real=1` -> gate routes to Phase 0
      orderly reboot (no in-place attempt).
- [ ] Soak both units under 64-channel load.
- [ ] Delete the test hook (five files), final commit.

## Phase 2 — bundle slave fw 2.9.7 (contingent)

Only if Phase 1 bench testing shows the 2.9.3 slave blocks reliable transport
reinit (the 2.9.4 changelog suggests reinit fixes may have touched both
sides). Build C6 `network_adapter.bin` at 2.9.7 with matching transport
config; update `SLAVE_FW_VERSION_*` in `slave_ota.c`; LOCKED 2.7.0 gate keeps
stuck devices untouched. **Caveat:** upgrade-only mechanism — no OTA
rollback; consider adding a force-reflash NVS flag to `slave_ota` first.

---

## Related backlog (not scheduled here)

- **SDIO RX mode streaming -> MAX_SIZE** — tabled fix in
  `docs/sdio-rx-oom-crash.md`; would also remove the streaming-frame-desync
  path implicated upstream (#184). Decision tracked there.
- Download pacing / concurrency cap in `dl_mgr` + `REFRESH_MAX_CONCURRENT`
  (also discussed in the OOM doc) — shrinks the trigger window.
- SDIO clock 40 -> 25/20 MHz experiment (margin, not a fix per #167).
- esp-hosted 2.12.x migration once `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`
  (2.12.8) lets us unpin from 2.9.x within the RAM budget.

---

## Log

- **2026-06-06** — Plan created after recurrence of the transport failure
  under 64-channel Giphy load. Phase 0 implementation started.
- **2026-06-06** — Phase 0 **done** (code complete). All changes from the
  Phase 0 table landed: `transport_recovery.c` (event handler, 5 s countdown
  reboot task, streak guard at 3, CP_INIT log-only telemetry), sdkconfig flag
  flipped, NVS counters + GOT_IP streak reset + `/status` field +
  OTA-validation reset wired. Verified `esp_hosted_event.h` is on the
  component's public include path and `ESP_HOSTED_EVENT` base is defined in
  the host lib (links). Not yet built/flashed — on-device validation
  checklist remains open.
- **2026-06-06** — Added temporary synthetic fault-injection hook
  (`TRANSPORT_FAULT_INJECT`, enabled in sdkconfig for the validation build):
  `POST /action/inject_transport_failure[?streak=N]`. Marked for deletion
  after Phase 0 validation; `grep -rn "TRANSPORT_FAULT_INJECT"` finds all
  four touched files.
- **2026-06-06** — First on-device test round (synthetic injection):
  **reboot path ✓** (countdown in logs, clean SW_CPU_RESET, wifi shutdown
  handlers ran), **degraded-mode path ✓** (`?streak=3` -> no reboot, playback
  continued). **On-screen UI ✗** — message state was set but never
  composited: during playback the animation renderer wins; the ugfx layer is
  only a last-resort fallback (`animation_player.c`). Fix: switch the render
  pipeline via `display_renderer_enter_ui_mode()` before showing the
  message, exactly like the touch-recovery countdown in `app_touch.c`
  (bounded: mode wait times out after 500 ms, so a wedged renderer cannot
  block the reboot). Degraded notice now shows ~5 s from a dedicated task,
  then hides and returns the screen to playback. Same fix applied to the
  pre-existing (latent) invisible countdown in `wifi_recovery.c`'s
  hard-reboot path. UI items on the checklist need a re-test.
- **2026-06-06** — Second test round: on-screen messages confirmed on both
  paths (countdown reboot and degraded notice). Checklist items checked. UI
  copy polished: terminal periods added to message lines (also in
  `wifi_recovery.c`'s countdown for consistency).
- **2026-06-06** — Extended the test hook with realistic injection
  (`?real=1`): pulses the C6 reset GPIO with the driver's own sequence
  (10 ms hold, polarity from Kconfig), so the SDIO driver detects the dead
  slave organically and the full production chain runs (driver errors ->
  event -> handler -> countdown -> reboot). Adds a temporary
  `esp_driver_gpio` dep to wifi_manager (marked; deletion list now five
  files).
- **2026-06-06** — Third test round (`?real=1&streak=3`, on the **2.7.0
  bench unit** — Phase 0 thereby validated on the locked-fleet
  representative): real driver-path detection worked end-to-end and fast
  (GPIO pulse at t+0 -> first CMD53 0x107 at +11 ms -> "Unrecoverable host
  sdio state" -> handler at +52 ms); degraded branch + on-screen notice +
  playback resume all correct. **Finding:** degraded mode left the stack
  live — endless sdio_write_task retry storms, MQTT reconnect loops, DNS
  health-check failures; the health monitor would eventually run futile
  full-reinit cycles (RPC-timeout storms) and could hard-reboot via its own
  streak, defeating the policy. **Fix (degraded-mode quiesce):** netif down +
  standard disconnect signals + MQTT stop from the notice task (inline
  netif-down fallback if the task can't spawn), health monitor stands down
  via `transport_recovery_is_degraded()`. Degraded is now quiet and terminal
  until reboot.
- **2026-06-06** — Fourth test round (after rebuild with the quiesce):
  confirmed the error storm is gone — degraded mode is quiet, playback-only.
  Remaining before closing Phase 0 validation: plain `?real=1` reboot
  variant on a clean streak, normal-boot log check, streak-reset-on-GOT_IP
  check, soak under 64-channel load, then delete the test hook.
- **2026-06-06** — Handoff revision: work moves to the workstation with the
  slave-2.9.3 bench unit. Added the Handoff section (state, commits,
  standing decisions, next actions), expanded Phase 1 from sketch to an
  implementation guide (API surface, code anchors, the cache-slave-version-
  at-boot gotcha, hang-protection escalation timer, policy decisions, its
  own validation checklist), deferred test-hook deletion to end of Phase 1,
  reclassified heartbeat as Phase 2 (fleet maxes at 2.9.3). All testing so
  far was on the 2.7.0 bench unit, built under ESP-IDF v5.5.1.
- **2026-06-08** — Closed the open **plain `?real=1` reboot variant** check on
  the 2.7.0 bench unit (clean streak). Full chain confirmed from the monitor
  log: GPIO-54 pulse (`streak_preset=-1`) -> first CMD53 `0x107` at +5 ms ->
  `transport_rec: ESP-Hosted transport failure` handler at +62 ms -> `display_
  renderer: Entering UI mode` -> "Transport-failure reboot in 5..1" countdown
  -> clean `rst:0xc (SW_CPU_RESET)` (`esp_restart_noos`) -> reconnect. After
  reboot `/status` reported `transport_recovery_reboots:1`, MQTT back to
  `online`. **Doc fix:** the status endpoint is registered at `/status`
  (`http_api.c:904`), not `/api/status` as written here — corrected throughout
  this file (no code referenced the wrong path). Remaining Phase 0 quick
  checks: normal-boot log line, streak-reset-on-GOT_IP (the formal post-
  degraded sequence), and the passive soak.
