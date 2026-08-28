#!/usr/bin/env python3
"""pull_frames.py -- incremental HTTP puller for the frame-trace ring (jitter work stream).

    python host/jitter-lab/pull_frames.py RUN-20260828-01 [--host http://p3a-fab.local] [--every 120] [--hours 4] [--once]

Writes into host/jitter-lab/runs/<RUN>/:
    frames.csv      all ring rows ever seen (device CSV format, one header)
    stats.jsonl     /api/debug/frames/stats snapshots with host timestamp
    device.jsonl    /api/state + /api/memory snapshots (no /config: it holds API keys)
    pull_state.json cursor (next_seq), counters, last error

Ring capacity is ~8192 entries; at 30 fps that is ~4 min of frames, so poll at
least every 2-3 minutes during soaks (default 120 s).
"""
import argparse
import json
import os
import sys
import time
import datetime as dt
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent

EXPECTED_HOSTNAME = "p3a-fab"   # the dev unit (Fab renamed it 2026-08-28); another p3a may answer at p3a.local


def assert_dev_unit(host: str):
    """Refuse to touch any device that is not the jitter dev unit."""
    r = requests.get(f"{host}/api/device-name", timeout=10)
    r.raise_for_status()
    hn = r.json().get("hostname")
    if hn != EXPECTED_HOSTNAME:
        raise SystemExit(f"REFUSING: {host} reports hostname {hn!r}, expected {EXPECTED_HOSTNAME!r} (wrong device?)")


def now_iso():
    return dt.datetime.now().isoformat(timespec="milliseconds")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--every", type=float, default=120.0)
    ap.add_argument("--hours", type=float, default=0.0, help="stop after this many hours (0 = forever)")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--from-head", action="store_true", help="start at the ring head (ignore entries recorded before the run)")
    ap.add_argument("--runs-dir", default=str(HERE / "runs"))
    a = ap.parse_args()
    assert_dev_unit(a.host)

    run_dir = Path(a.runs_dir) / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    frames_csv = run_dir / "frames.csv"
    stats_path = run_dir / "stats.jsonl"
    device_path = run_dir / "device.jsonl"
    st_path = run_dir / "pull_state.json"

    st = {"pid": os.getpid(), "run": a.run, "host": a.host, "started": now_iso(),
          "next_seq": 0, "pulls": 0, "rows": 0, "errors": 0, "last_ok": None, "last_error": None,
          "epoch": 0}
    if st_path.exists():
        try:
            old = json.loads(st_path.read_text(encoding="utf-8"))
            st["next_seq"] = int(old.get("next_seq", 0))
            st["rows"] = int(old.get("rows", 0))
            st["epoch"] = int(old.get("epoch", 0))
        except Exception:
            pass

    def epoch_csv():
        # Epoch 0 is frames.csv; a device reboot (ring sequence restarts) opens frames.e<N>.csv.
        return frames_csv if st["epoch"] == 0 else run_dir / f"frames.e{st['epoch']}.csv"

    if a.from_head and st["next_seq"] == 0:
        try:
            head = requests.get(f"{a.host}/api/debug/frames/stats", timeout=15).json()["data"]["next_seq"]
            st["next_seq"] = int(head)
            print(f"{now_iso()} starting from ring head seq={head}", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"{now_iso()} from-head failed ({e}); starting from 0", flush=True)

    def save():
        tmp = st_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(st, indent=1), encoding="utf-8")
        os.replace(tmp, st_path)

    deadline = time.time() + a.hours * 3600 if a.hours > 0 else None
    n = 0
    while True:
        n += 1
        try:
            # Reboot detection: the device ring restarts at seq 1 after a reset.
            head = requests.get(f"{a.host}/api/debug/frames/stats", timeout=15).json()["data"]["next_seq"]
            if int(head) < st["next_seq"] - 8:
                st["epoch"] += 1
                st["next_seq"] = 0
                with (run_dir / "boots.jsonl").open("a", encoding="utf-8") as f:
                    f.write(json.dumps({"host_ts": now_iso(), "epoch": st["epoch"], "device_head": int(head)}) + "\n")
                print(f"{now_iso()} DEVICE REBOOT detected: epoch {st['epoch']}, cursor reset", flush=True)
            r = requests.get(f"{a.host}/api/debug/frames", params={"since": st["next_seq"]}, timeout=60)
            r.raise_for_status()
            text = r.text
            lines = text.splitlines()
            header = lines[0] if lines else ""
            rows = [ln for ln in lines[1:] if ln and not ln.startswith("#")]
            nxt = None
            for ln in lines:
                if ln.startswith("#next,"):
                    nxt = int(ln.split(",", 1)[1])
            out_csv = epoch_csv()
            write_header = not out_csv.exists() or out_csv.stat().st_size == 0
            with out_csv.open("a", encoding="utf-8", newline="\n") as f:
                if write_header and header:
                    f.write(header + "\n")
                for ln in rows:
                    f.write(ln + "\n")
            if nxt is not None:
                st["next_seq"] = nxt
            st["rows"] += len(rows)
            st["pulls"] += 1
            st["last_ok"] = now_iso()

            s = requests.get(f"{a.host}/api/debug/frames/stats", timeout=15).json()
            with stats_path.open("a", encoding="utf-8") as f:
                f.write(json.dumps({"host_ts": now_iso(), "stats": s.get("data", s)}) + "\n")

            if n % 5 == 1:
                dev = {"host_ts": now_iso()}
                for ep in ("/api/state", "/api/memory?silent"):
                    try:
                        dev[ep] = requests.get(f"{a.host}{ep}", timeout=15).json().get("data")
                    except Exception as e:  # noqa: BLE001
                        dev[ep] = f"error: {e}"
                with device_path.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(dev) + "\n")
            print(f"{now_iso()} pull #{st['pulls']} rows+={len(rows)} next_seq={st['next_seq']} "
                  f"stalls_hard={s.get('data', {}).get('stalls_hard')}", flush=True)
        except Exception as e:  # noqa: BLE001
            st["errors"] += 1
            st["last_error"] = f"{now_iso()} {e}"
            print(f"{now_iso()} pull error: {e}", flush=True)
        save()
        if a.once or (deadline and time.time() >= deadline):
            return 0
        time.sleep(a.every)


if __name__ == "__main__":
    sys.exit(main())
