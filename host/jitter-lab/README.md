# jitter-lab — host tooling for the jitter work stream

See `docs/jitter/README.md` for the work stream, rules and resume protocol.
All tools are plain Python 3 (system `python`, needs `pyserial` + `requests`,
both present on the laptop) or PowerShell 7.

## Facts every tool must respect

- Dev device UART: **COM5**, 115200. `serial_logger.py` opens it with DTR/RTS
  pre-set low and **does not reset the board** (verified 2026-08-28: uptime kept
  climbing across a logger start). .NET `SerialPort` and `idf.py monitor`
  without `--no-reset` DO reset it. `idf.py flash` always resets (by design).
- Device HTTP: `http://p3a.local`. `GET /config` contains API keys:
  `snapshot_settings.py` redacts every `*_api_key`; nothing else reads it.
- Raw run data goes to `host/jitter-lab/runs/<RUN-ID>/` (gitignored).
  Committed summaries go to `docs/jitter/runs/<RUN-ID>.md`.
- Run IDs: `RUN-YYYYMMDD-NN` (`soak.ps1 -NewRunId` prints the next free one).
- Ring capacity is 8192 entries (~4 min of 30 fps frames + marks): the puller
  must poll at least every 2-3 minutes or rows are lost.

## Tools

| Tool | Purpose |
|------|---------|
| `build.ps1 [-Diag] [-Flash] [-FullClean] [-Port COM5]` | Activates IDF, builds release (`build/`) or diag (`build-diag/`, release sdkconfig + `sdkconfig.diag.defaults`). Guards: release `sdkconfig` unchanged, P4 rev-v1.0 lines present, trace on/off as expected. `-Flash` flashes the dev unit (stop any logger first: flashing needs COM5). |
| `serial_logger.py COM5 RUN` | Persistent reset-free UART logger → `uart.log`, parsed `JTR\|` stall reports → `stalls.jsonl`, heartbeat `state.json`. Reconnects on error. |
| `pull_frames.py RUN [--every 120] [--hours N] [--once]` | Incremental `GET /api/debug/frames?since=` → `frames.csv`; `stats.jsonl`; `device.jsonl` (`/api/state`, `/api/memory`). Cursor in `pull_state.json`. |
| `snapshot_settings.py save\|restore\|set\|show RUN` | Redacted settings snapshot before a run; restore of show_fps / max_speed / brightness / rotation / dwell / active playset afterwards. |
| `analyze.py RUN` | `frames.csv` + `stalls.jsonl` → `report.md` + `summary.json`: lateness percentiles/histogram, stall list with mark attribution and UART task deltas, overrun (out-of-scope) separation. |
| `soak.ps1 -Start/-Status/-Stop -Run RUN` | Detached logger + puller (survive the agent session), pids in `pids.json`; `-Stop` does a final pull and runs `analyze.py`. |
| `find_port.ps1` | Read-only COM probe. **Resets the board.** Last resort. |

## Device endpoints added by the diag build (`CONFIG_P3A_FRAME_TRACE`)

```
GET  /api/debug/frames?since=<seq>    CSV; last line "#next,<seq>"
GET  /api/debug/frames/stats          JSON aggregates, histogram, kind names, config
POST /api/debug/frames/reset          restart aggregates (ring kept)
POST /api/debug/mark?arg=<n>          user marker (run boundaries)
POST /api/debug/provoke?kind=nvs|log|sd|cpu1&n=<n>   dev-only (CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS)
```

UART report format (one block per stall, rate-limited 1 per 5 s):

```
JTR|STALL seq=<seq> t_us=<present> lateness_ms=<n> margin_ms=<n> dur_ms=<n> flags=0x..
JTR|F <seq> <t_us> <lateness_us> <margin_us> <produce_us> <decode_us> <upscale_us> <free_wait_us> <vsync_wait_us> <dur_ms> <qd> <flags> <gen>
JTR|M <seq> <t_us> <kind> <phase> <arg> <core> <task_tag>
JTR|T window_us=<n> tasks=<n>            then one line per task that used >= 0.5 % of the window:
JTR|T <name> core=<c> prio=<p> run_us=<delta> state=<s>
JTR|END
```

Caveat: `JTR|T` deltas compare the current task list with the reporter's
last periodic snapshot (≤ 1 s old); a task that was created and deleted inside
the window (e.g. the `cpu1` provoke hog) does not appear. Persistent culprits do.

## Typical session

```powershell
pwsh host/jitter-lab/build.ps1 -Diag -Flash            # needs COM5 free
$run = pwsh host/jitter-lab/soak.ps1 -NewRunId
pwsh host/jitter-lab/soak.ps1 -Start -Run $run -Hours 4 -Note "baseline, Work mix"
pwsh host/jitter-lab/soak.ps1 -Status -Run $run
pwsh host/jitter-lab/soak.ps1 -Stop -Run $run            # final pull + analyze -> runs/$run/report.md
python host/jitter-lab/snapshot_settings.py restore $run
```
