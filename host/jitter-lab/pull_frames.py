#!/usr/bin/env python3
"""pull_frames.py -- incremental HTTP puller for the frame-trace ring (jitter work stream).

    python host/jitter-lab/pull_frames.py RUN-20260828-01 [--host http://p3a.local] [--every 120] [--hours 4] [--once]

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


def now_iso():
    return dt.datetime.now().isoformat(timespec="milliseconds")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a.local")
    ap.add_argument("--every", type=float, default=120.0)
    ap.add_argument("--hours", type=float, default=0.0, help="stop after this many hours (0 = forever)")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--runs-dir", default=str(HERE / "runs"))
    a = ap.parse_args()

    run_dir = Path(a.runs_dir) / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    frames_csv = run_dir / "frames.csv"
    stats_path = run_dir / "stats.jsonl"
    device_path = run_dir / "device.jsonl"
    st_path = run_dir / "pull_state.json"

    st = {"pid": os.getpid(), "run": a.run, "host": a.host, "started": now_iso(),
          "next_seq": 0, "pulls": 0, "rows": 0, "errors": 0, "last_ok": None, "last_error": None}
    if st_path.exists():
        try:
            old = json.loads(st_path.read_text(encoding="utf-8"))
            st["next_seq"] = int(old.get("next_seq", 0))
            st["rows"] = int(old.get("rows", 0))
        except Exception:
            pass

    def save():
        tmp = st_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(st, indent=1), encoding="utf-8")
        os.replace(tmp, st_path)

    deadline = time.time() + a.hours * 3600 if a.hours > 0 else None
    n = 0
    while True:
        n += 1
        try:
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
            write_header = not frames_csv.exists() or frames_csv.stat().st_size == 0
            with frames_csv.open("a", encoding="utf-8", newline="\n") as f:
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
