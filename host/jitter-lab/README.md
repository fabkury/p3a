# jitter-lab — host tooling for the jitter work stream

See `docs/jitter/README.md` for the work stream, rules and resume protocol.

Status: **skeleton (Phase 0).** Real tooling lands in Phase 2 (see
`docs/jitter/PLAN.md` §5): `serial_logger.py`, `pull_frames.py`,
`snapshot_settings.py`, `analyze.py`, `soak.ps1`.

## Facts every tool must respect

- Dev device UART: **COM5**, 115200. Opening the port with .NET `SerialPort`
  **resets the board**. Hold one persistent connection; open with DTR/RTS low
  (pyserial: set `dtr=False`, `rts=False` before `open()`), or use
  `idf.py monitor --no-reset`. Verify on first use and record the result in
  `docs/jitter/LOG.md`.
- Device HTTP: `http://p3a.local`. `GET /config` contains API keys: redact
  every `*_api_key` field before anything is written to disk.
- Raw run data goes to `host/jitter-lab/runs/<RUN-ID>/` (gitignored).
  Committed summaries go to `docs/jitter/runs/<RUN-ID>.md`.
- Run IDs: `RUN-YYYYMMDD-NN`.

## find_port.ps1

Read-only probe of candidate COM ports for ESP-IDF boot/log traffic. Last
resort only, because opening a port resets whatever ESP is behind it.

```powershell
pwsh host/jitter-lab/find_port.ps1            # probes all ports
pwsh host/jitter-lab/find_port.ps1 COM5,COM12 # probes a subset
```
