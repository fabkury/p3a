#!/usr/bin/env python3
"""provoke_experiment.py -- generic provocation matrix runner (jitter work stream, Phase 4).

    python host/jitter-lab/provoke_experiment.py RUN-ID --host http://p3a-fab.local --rounds 2 \
        --cond "msync kb=1 buf=0" --cond "msync kb=32 buf=0" --cond "msync kb=1 buf=2" --cond "spin"

Each --cond is "<kind> key=value ..." passed to POST /api/debug/provoke (n = --ms for
task-style kinds). Conditions are run in order, `rounds` times, with `gap` s between.
Per condition: frames inside the provoke interval, upscale median/p90/max, producer
anomalies (produce >= 3x the artwork's median over the capture), anomaly rate.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--ms", type=int, default=4000)
    ap.add_argument("--gap", type=float, default=8.0)
    ap.add_argument("--cond", action="append", required=True)
    a = ap.parse_args()
    hn = requests.get(f"{a.host}/api/device-name", timeout=10).json().get("hostname")
    if hn != "p3a-fab":
        raise SystemExit(f"REFUSING: {a.host} is {hn!r}, not p3a-fab")
    run_dir = HERE / "runs" / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    head = requests.get(f"{a.host}/api/debug/frames/stats", timeout=15).json()["data"]["next_seq"]
    (run_dir / "pull_state.json").write_text(json.dumps({"next_seq": int(head), "rows": 0, "epoch": 0}), encoding="utf-8")

    def pull():
        subprocess.run([sys.executable, str(HERE / "pull_frames.py"), a.run, "--host", a.host, "--once"], check=False, stdout=subprocess.DEVNULL)

    conds = []
    for c in a.cond:
        parts = c.split()
        params = {"kind": parts[0], "n": a.ms}
        for kv in parts[1:]:
            k, v = kv.split("=", 1)
            params[k] = v
        conds.append((c, params))

    time.sleep(a.gap)
    for r in range(a.rounds):
        for label, params in conds:
            resp = requests.post(f"{a.host}/api/debug/provoke", params=params, timeout=120)
            print(f"round {r} {label}: HTTP {resp.status_code}", flush=True)
            time.sleep(a.ms / 1000 + a.gap)
            pull()
    pull()

    rows = list(csv.DictReader((run_dir / "frames.csv").open(encoding="utf-8")))
    rows = list({(x["seq"], x["t_us"]): x for x in rows}.values())
    frames = [x for x in rows if x["type"] == "F" and not (int(x["flags"]) & 3)]
    marks = sorted([x for x in rows if x["type"] == "M" and x["kind"] == "provoke"], key=lambda x: int(x["t_us"]))
    by_gen = defaultdict(list)
    for f in frames:
        by_gen[f["arg"]].append(int(f["produce_us"]))
    med = {g: statistics.median(v) for g, v in by_gen.items()}
    # intervals in order: BEGIN then END (END arg may differ: it carries a count for some kinds)
    intervals, t0 = [], None
    for m in marks:
        if m["phase"] == "1":
            t0 = int(m["t_us"])
        elif m["phase"] == "2" and t0 is not None:
            intervals.append((t0, int(m["t_us"]))); t0 = None
    labels = [label for _ in range(a.rounds) for label, _ in conds]
    agg = defaultdict(lambda: {"ups": [], "anom": 0, "n": 0, "dur": 0})
    for i, (t0, t1) in enumerate(intervals):
        lab = labels[i] if i < len(labels) else f"interval {i}"
        fr = [f for f in frames if t0 <= int(f["t_us"]) <= t1]
        g = agg[lab]
        g["ups"] += [int(f["upscale_us"]) for f in fr]
        g["anom"] += sum(1 for f in fr if int(f["produce_us"]) >= 3 * max(med.get(f["arg"], 0), 1000))
        g["n"] += len(fr); g["dur"] += (t1 - t0)
    inside = lambda t: any(t0 - 200000 <= t <= t1 + 400000 for t0, t1 in intervals)
    base = [f for f in frames if not inside(int(f["t_us"]))]
    base_anom = sum(1 for f in base if int(f["produce_us"]) >= 3 * max(med.get(f["arg"], 0), 1000))
    out = ["| condition | frames | upscale med / p90 / max ms | anomalies | rate |", "|---|---|---|---|---|"]
    for lab in dict.fromkeys(labels):
        g = agg.get(lab)
        if not g or not g["ups"]:
            out.append(f"| {lab} | 0 | | | |"); continue
        u = sorted(g["ups"])
        out.append(f"| {lab} | {g['n']} | {statistics.median(u)/1000:.1f} / {u[int(.9*len(u))-1]/1000:.1f} / {u[-1]/1000:.0f} | {g['anom']} | {100*g['anom']/g['n']:.1f}% |")
    u = sorted(int(f["upscale_us"]) for f in base)
    out.append(f"| baseline (outside) | {len(base)} | {statistics.median(u)/1000:.1f} / {u[int(.9*len(u))-1]/1000:.1f} / {u[-1]/1000:.0f} | {base_anom} | {100*base_anom/max(len(base),1):.1f}% |")
    text = "\n".join(out)
    print(text)
    (run_dir / "experiment.md").write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
