#!/usr/bin/env python3
"""zm_reproducer.py -- zero-margin artwork + paced SD writes, one arm at a time (esp-idf #19034 evaluation).

    python host/jitter-lab/zm_reproducer.py RUN --arm wrap|patch|stock [--gif noise_360_60ms.gif] [--rounds 3]

The RUN-20260902-06 stall class: an artwork whose decode nearly fills its frame
period (zero margin) while the download task writes 32 KB chunks that keep the
card busy for tens of ms. The bar-GIF reproducer cannot see it (16 ms upscale,
tiny decode). This one:
  1. checks the arm identity on the device (like arm_reproducer.py)
  2. uploads an incompressible noise GIF (make_test_anim.py --noise), dwell 3600
  3. calibrates: median produce/decode of the playing artwork vs its frame period
  4. runs, `rounds` times, in order:
       A  aligned PSRAM 32 KB x 64  (multi-block writes, no bounce, no storm: the
          card is busy 5-45 ms per write, the busy-wait is what differs per arm)
       B  misaligned PSRAM 32 KB x 8 (the poll-storm positive control)
       C  internal 512 B x 512       (short busy periods, many commands)
  5. per condition: frames in window, decode / produce median and max, lateness
     max, producer anomalies (>= 3x median or >= 1 frame period above), and
     frames late >= 100 ms
  6. restores dwell and the soak playset
Writes runs/<RUN>/zm_experiment.md and zm.json.
"""
import argparse
import csv
import json
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
ARMS = {"stock": (False, False), "patch": (False, True), "wrap": (True, False)}
CONDS = [("A aligned 32KB x64", {"kind": "sd", "buf": 1, "chunk": 32768, "n": 64}),
         ("B misaligned 32KB x8", {"kind": "sd", "buf": 0, "chunk": 32768, "n": 8}),
         ("C internal 512B x512", {"kind": "sd", "buf": 2, "chunk": 512, "n": 512})]


def pull(run, host):
    subprocess.run([sys.executable, str(HERE / "pull_frames.py"), run, "--host", host, "--once"], check=False,
                   stdout=subprocess.DEVNULL)


def load(run_dir):
    rows = list(csv.DictReader((run_dir / "frames.csv").open(encoding="utf-8")))
    rows = list({(x["seq"], x["t_us"]): x for x in rows}.values())
    # Keep BASELINED / MAX_SPEED frames too: decode and produce times are what
    # this probe measures; lateness is only counted on unflagged frames.
    frames = [x for x in rows if x["type"] == "F"]
    marks = [x for x in rows if x["type"] == "M"]
    return frames, marks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--arm", choices=sorted(ARMS), required=True)
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--gif", default="noise_360_60ms.gif")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--gap", type=float, default=12.0)
    ap.add_argument("--settle", type=float, default=25.0)
    ap.add_argument("--playset", default="Work mix")
    ap.add_argument("--dwell", type=int, default=3600,
                    help="dwell seconds; a short dwell on the single-artwork playset makes the loader re-read the file every dwell (loader traffic variant)")
    ap.add_argument("--no-restore", action="store_true")
    a = ap.parse_args()
    H = a.host
    hn = requests.get(f"{H}/api/device-name", timeout=10).json().get("hostname")
    if hn != "p3a-fab":
        raise SystemExit(f"REFUSING: {H} is {hn!r}, not p3a-fab")
    cfg = requests.get(f"{H}/api/debug/frames/stats", timeout=15).json()["data"]["config"]
    ident = (bool(cfg.get("sd_idle_wait_wrap")), bool(cfg.get("idf_sdmmc_backoff_patch")))
    if ident != ARMS[a.arm]:
        raise SystemExit(f"ARM MISMATCH: device wrap={ident[0]} patch={ident[1]}, --arm {a.arm} expects {ARMS[a.arm]}")
    if not cfg.get("dev_endpoints"):
        raise SystemExit("device build has no provoke endpoints (diag overlay needed)")
    run_dir = HERE / "runs" / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    py = sys.executable
    print(f"arm {a.arm} confirmed: {cfg}", flush=True)

    subprocess.run([py, str(HERE / "snapshot_settings.py"), "save", a.run, "--host", H], check=True)
    gif = HERE / "runs" / "test-anims" / a.gif
    if not gif.exists():
        raise SystemExit(f"missing {gif}: make_test_anim.py --noise ...")
    with gif.open("rb") as f:
        r = requests.post(f"{H}/upload", files={"file": (gif.name, f, "image/gif")}, timeout=180)
    print("upload ->", r.status_code, r.text[:100], flush=True)
    r.raise_for_status()
    subprocess.run([py, str(HERE / "snapshot_settings.py"), "set", a.run, "--host", H, "--dwell", str(a.dwell)], check=True)
    time.sleep(a.settle)

    # start the capture at the ring head, then calibrate on a few seconds of quiet playback
    head = requests.get(f"{H}/api/debug/frames/stats", timeout=15).json()["data"]["next_seq"]
    (run_dir / "pull_state.json").write_text(json.dumps({"next_seq": int(head), "rows": 0, "epoch": 0}), encoding="utf-8")
    time.sleep(10)
    pull(a.run, H)
    frames, _ = load(run_dir)
    if len(frames) < 20:
        raise SystemExit("too few frames captured for calibration")
    gen = statistics.mode(f["arg"] for f in frames[-100:])
    quiet = [f for f in frames if f["arg"] == gen]
    dur_ms = statistics.median(int(f["duration_ms"]) for f in quiet)
    prod = statistics.median(int(f["produce_us"]) for f in quiet) / 1000
    dec = statistics.median(int(f["decode_us"]) for f in quiet) / 1000
    flagged = sum(1 for f in quiet if int(f["flags"]) & 3)
    calib = {"gen": gen, "frame_ms": dur_ms, "produce_med_ms": round(prod, 1), "decode_med_ms": round(dec, 1),
             "margin_ms": round(dur_ms - prod, 1), "frames": len(quiet), "baselined_or_maxspeed": flagged}
    print("calibration:", calib, flush=True)

    labels = []
    try:
        for rnd in range(a.rounds):
            for label, params in CONDS:
                t0 = time.time()
                resp = requests.post(f"{H}/api/debug/provoke", params=params, timeout=180)
                print(f"round {rnd} {label}: HTTP {resp.status_code} in {time.time()-t0:.1f}s", flush=True)
                labels.append(label)
                time.sleep(a.gap)
                pull(a.run, H)
        pull(a.run, H)
    finally:
        if not a.no_restore:
            subprocess.run([py, str(HERE / "snapshot_settings.py"), "restore", a.run, "--host", H], check=False)
            subprocess.run([py, str(HERE / "snapshot_settings.py"), "set", a.run, "--host", H, "--playset", a.playset], check=False)

    frames, marks = load(run_dir)
    if a.dwell >= 600:
        frames = [f for f in frames if f["arg"] == gen]
    else:
        # short dwell: every reload of the same file starts a new generation; keep
        # frames of this file (same frame period as the calibrated one)
        frames = [f for f in frames if int(f["duration_ms"]) == int(dur_ms)]
    loads = sum(1 for m in marks if m["kind"] == "loader_load" and m["phase"] == "2")
    pm = sorted([m for m in marks if m["kind"] == "provoke"], key=lambda x: int(x["t_us"]))
    intervals, t0 = [], None
    for m in pm:
        if m["phase"] == "1":
            t0 = int(m["t_us"])
        elif m["phase"] == "2" and t0 is not None:
            intervals.append((t0, int(m["t_us"]))); t0 = None
    med_p = statistics.median(int(f["produce_us"]) for f in frames)
    agg = defaultdict(lambda: {"n": 0, "dec": [], "prod": [], "late": [], "anom": 0, "late100": 0, "dur": 0})
    for i, (ta, tb) in enumerate(intervals):
        lab = labels[i] if i < len(labels) else f"interval {i}"
        fr = [f for f in frames if ta <= int(f["t_us"]) <= tb + 300_000]
        g = agg[lab]
        g["n"] += len(fr); g["dur"] += tb - ta
        g["dec"] += [int(f["decode_us"]) for f in fr]
        g["prod"] += [int(f["produce_us"]) for f in fr]
        g["late"] += [int(f["lateness_us"]) for f in fr if not (int(f["flags"]) & 3)] or [0]
        g["anom"] += sum(1 for f in fr if int(f["produce_us"]) >= max(3 * med_p, med_p + dur_ms * 1000))
        g["late100"] += sum(1 for f in fr if int(f["lateness_us"]) >= 100_000 and not (int(f["flags"]) & 3))
    inside = lambda t: any(ta - 300_000 <= t <= tb + 600_000 for ta, tb in intervals)
    base = [f for f in frames if not inside(int(f["t_us"]))]
    out = [f"arm **{a.arm}**, artwork {a.gif}, dwell {a.dwell} s ({loads} loader loads during the capture): frame {dur_ms:.0f} ms, produce median {prod:.1f} ms (decode {dec:.1f}), margin {dur_ms - prod:.1f} ms",
           "", "| condition | frames | decode med / max ms | produce med / max ms | lateness max ms | anomalies | late >= 100 ms |",
           "|---|---|---|---|---|---|---|"]
    for lab in dict.fromkeys(labels):
        g = agg.get(lab)
        if not g or not g["n"]:
            out.append(f"| {lab} | 0 | | | | | |"); continue
        out.append(f"| {lab} | {g['n']} | {statistics.median(g['dec'])/1000:.1f} / {max(g['dec'])/1000:.0f} | "
                   f"{statistics.median(g['prod'])/1000:.1f} / {max(g['prod'])/1000:.0f} | {max(g['late'])/1000:.0f} | {g['anom']} | {g['late100']} |")
    if base:
        out.append(f"| baseline (outside) | {len(base)} | {statistics.median(int(f['decode_us']) for f in base)/1000:.1f} / {max(int(f['decode_us']) for f in base)/1000:.0f} | "
                   f"{statistics.median(int(f['produce_us']) for f in base)/1000:.1f} / {max(int(f['produce_us']) for f in base)/1000:.0f} | "
                   f"{max([int(f['lateness_us']) for f in base if not (int(f['flags']) & 3)] or [0])/1000:.0f} | {sum(1 for f in base if int(f['produce_us']) >= max(3*med_p, med_p + dur_ms*1000))} | "
                   f"{sum(1 for f in base if int(f['lateness_us']) >= 100_000 and not (int(f['flags']) & 3))} |")
    text = "\n".join(out)
    print(text)
    (run_dir / "zm_experiment.md").write_text(text + "\n", encoding="utf-8")
    (run_dir / "zm.json").write_text(json.dumps({"arm": a.arm, "gif": a.gif, "calibration": calib, "device_config": cfg,
                                                 "rounds": a.rounds}, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
