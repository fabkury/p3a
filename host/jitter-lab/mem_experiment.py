#!/usr/bin/env python3
"""mem_experiment.py -- Phase 4 provocation: does CPU memory traffic on the other core
(shared L2 / PSRAM bandwidth) slow the upscale? Control: a busy-spin with no memory traffic.

    python host/jitter-lab/mem_experiment.py RUN-ID [--host http://p3a-fab.local] [--rounds 2] [--ms 4000]

Conditions: kind=mem kb in {64, 256, 1024} at prio 4 on core 0 (like event_bus),
kind=mem 256 KB at prio 4 on core 1, kind=spin (prio 4 core 0). Reports per
condition: frames in window, upscale median/max, producer anomalies.
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
CONDS = [  # (kind, kb, prio, core)
    ("mem", 64, 4, 0), ("mem", 256, 4, 0), ("mem", 1024, 4, 0),
    ("mem", 256, 4, 1), ("spin", 0, 4, 0), ("mem", 256, 6, 0),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--ms", type=int, default=4000)
    ap.add_argument("--gap", type=float, default=8.0)
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

    time.sleep(a.gap)
    for r in range(a.rounds):
        for kind, kb, prio, core in CONDS:
            resp = requests.post(f"{a.host}/api/debug/provoke", params={"kind": kind, "n": a.ms, "kb": kb, "prio": prio, "core": core}, timeout=30)
            print(f"round {r} {kind} kb={kb} prio={prio} core={core}: HTTP {resp.status_code}", flush=True)
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
    intervals, open_ = [], {}
    for m in marks:
        code = int(m["arg"])
        if m["phase"] == "1":
            open_[code] = int(m["t_us"])
        elif m["phase"] == "2" and code in open_:
            intervals.append((code, open_.pop(code), int(m["t_us"])))
    # condition label is (code, order index); prio/core not in the code -> use sequence order per round
    labels = []
    for r in range(a.rounds):
        labels += [f"{k} {kb}KB prio{p} core{c}" for k, kb, p, c in CONDS]
    agg = defaultdict(lambda: {"ups": [], "anom": 0, "n": 0})
    for i, (code, t0, t1) in enumerate(intervals):
        lab = labels[i] if i < len(labels) else f"code {code:x}"
        fr = [f for f in frames if t0 <= int(f["t_us"]) <= t1]
        g = agg[lab]
        g["ups"] += [int(f["upscale_us"]) for f in fr]
        g["anom"] += sum(1 for f in fr if int(f["produce_us"]) >= 3 * max(med.get(f["arg"], 0), 1000))
        g["n"] += len(fr)
    inside = lambda t: any(t0 - 200000 <= t <= t1 + 400000 for _, t0, t1 in intervals)
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
    (run_dir / "mem_experiment.md").write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
