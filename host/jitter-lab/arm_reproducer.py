#!/usr/bin/env python3
"""arm_reproducer.py -- the CMD13 poll-storm reproducer, one arm at a time (esp-idf #19034 evaluation).

    python host/jitter-lab/arm_reproducer.py RUN-20260902-01 --arm patch [--rounds 3] [--host http://p3a-fab.local]

Procedure (identical for every arm, so the arms compare):
  1. refuse anything but the dev unit; read /api/debug/frames/stats config
     (sd_idle_wait_wrap, idf_sdmmc_backoff_patch) and CHECK it matches --arm
     (stock = wrap off + no patch, patch = wrap off + patch, wrap = wrap on + no patch)
  2. snapshot settings; upload bar_30fps.gif (CPU-upscaled, constant decode
     cost) which starts playing it; dwell 3600 s so it stays
  3. run sd_experiment.py (PSRAM-misaligned / aligned / internal x 512 B / 32 KB,
     --rounds rounds): the misaligned 32 KB condition is the poll-storm reproducer
  4. restore settings (dwell, playset)
Writes runs/<RUN>/arm.json with the arm identity; sd_experiment.md is produced
by sd_experiment.py in the same run dir.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
ARMS = {"stock": (False, False), "patch": (False, True), "wrap": (True, False)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--arm", choices=sorted(ARMS), required=True)
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--settle", type=float, default=20.0, help="seconds after the upload before provoking")
    ap.add_argument("--no-restore", action="store_true")
    a = ap.parse_args()
    H = a.host
    hn = requests.get(f"{H}/api/device-name", timeout=10).json().get("hostname")
    if hn != "p3a-fab":
        raise SystemExit(f"REFUSING: {H} is {hn!r}, not p3a-fab")
    cfg = requests.get(f"{H}/api/debug/frames/stats", timeout=15).json()["data"]["config"]
    ident = (bool(cfg.get("sd_idle_wait_wrap")), bool(cfg.get("idf_sdmmc_backoff_patch")))
    want = ARMS[a.arm]
    if ident != want:
        raise SystemExit(f"ARM MISMATCH: device reports wrap={ident[0]} patch={ident[1]}, "
                         f"--arm {a.arm} expects wrap={want[0]} patch={want[1]}")
    if not cfg.get("dev_endpoints"):
        raise SystemExit("device build has no provoke endpoints (needs the diag overlay)")
    run_dir = HERE / "runs" / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    print(f"arm {a.arm} confirmed on device: {cfg}", flush=True)

    py = sys.executable
    subprocess.run([py, str(HERE / "snapshot_settings.py"), "save", a.run, "--host", H], check=True)
    gif = HERE / "runs" / "test-anims" / "bar_30fps.gif"
    if not gif.exists():
        raise SystemExit(f"missing {gif}: run make_test_anim.py first")
    with gif.open("rb") as f:
        r = requests.post(f"{H}/upload", files={"file": ("bar_30fps.gif", f, "image/gif")}, timeout=120)
    print("upload ->", r.status_code, r.text[:120], flush=True)
    r.raise_for_status()
    subprocess.run([py, str(HERE / "snapshot_settings.py"), "set", a.run, "--host", H, "--dwell", "3600"], check=True)
    time.sleep(a.settle)
    try:
        subprocess.run([py, str(HERE / "sd_experiment.py"), a.run, "--host", H, "--rounds", str(a.rounds)], check=True)
    finally:
        if not a.no_restore:
            subprocess.run([py, str(HERE / "snapshot_settings.py"), "restore", a.run, "--host", H], check=False)
    (run_dir / "arm.json").write_text(json.dumps({"arm": a.arm, "device_config": cfg, "rounds": a.rounds,
                                                  "finished": time.strftime("%Y-%m-%dT%H:%M:%S")}, indent=1),
                                      encoding="utf-8")
    print(f"done: {run_dir / 'sd_experiment.md'}")


if __name__ == "__main__":
    main()
