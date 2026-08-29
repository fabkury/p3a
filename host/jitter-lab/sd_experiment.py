#!/usr/bin/env python3
"""sd_experiment.py -- Phase 4 provocation: does an in-flight SD write slow the CPU upscale,
and does it depend on where the buffer lives? (jitter work stream, H3b/H4)

    python host/jitter-lab/sd_experiment.py RUN-ID [--host http://p3a-fab.local] [--rounds 3]

Runs a matrix of POST /api/debug/provoke?kind=sd&buf=<mode>&chunk=<bytes>&n=<count>
(mode 0 PSRAM misaligned -> bounce path, 1 PSRAM aligned -> direct DMA, 2 internal
RAM -> direct DMA never touching PSRAM), pulls the ring, and reports per condition:
frames inside the provoke interval, upscale median/max, producer anomalies
(produce >= 3x the artwork's median over the whole capture), sd_write durations.
Needs the diag build with CONFIG_P3A_FRAME_TRACE_DEV_ENDPOINTS. Does not touch
settings; whatever is playing keeps playing.
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
MATRIX = [  # (mode, chunk, n) -> ~256 KB per condition
    (0, 512, 512), (0, 32768, 8),
    (1, 512, 512), (1, 32768, 8),
    (2, 512, 512), (2, 32768, 8),
]
NAMES = {0: "psram-misaligned", 1: "psram-aligned", 2: "internal"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--gap", type=float, default=10.0)
    a = ap.parse_args()

    hn = requests.get(f"{a.host}/api/device-name", timeout=10).json().get("hostname")
    if hn != "p3a-fab":
        raise SystemExit(f"REFUSING: {a.host} is {hn!r}, not p3a-fab")

    run_dir = HERE / "runs" / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    # start from the ring head
    head = requests.get(f"{a.host}/api/debug/frames/stats", timeout=15).json()["data"]["next_seq"]
    st_path = run_dir / "pull_state.json"
    st_path.write_text(json.dumps({"next_seq": int(head), "rows": 0, "epoch": 0}), encoding="utf-8")
    log = (run_dir / "experiment.log").open("a", encoding="utf-8")

    def pull():
        subprocess.run([sys.executable, str(HERE / "pull_frames.py"), a.run, "--host", a.host, "--once"], check=False,
                       stdout=subprocess.DEVNULL)

    print(f"warm-up {a.gap}s ...", flush=True)
    time.sleep(a.gap)
    for r in range(a.rounds):
        for mode, chunk, n in MATRIX:
            code = (mode << 24) | chunk
            t0 = time.time()
            resp = requests.post(f"{a.host}/api/debug/provoke", params={"kind": "sd", "buf": mode, "chunk": chunk, "n": n}, timeout=120)
            dt = time.time() - t0
            line = f"round {r} mode {mode} ({NAMES[mode]}) chunk {chunk} n {n}: HTTP {resp.status_code} in {dt:.1f}s"
            print(line, flush=True); log.write(line + "\n"); log.flush()
            time.sleep(a.gap)
            pull()  # keep the ring from overflowing between conditions
    pull()

    # ---- analysis
    rows = list(csv.DictReader((run_dir / "frames.csv").open(encoding="utf-8")))
    rows = list({(x["seq"], x["t_us"]): x for x in rows}.values())
    frames = [x for x in rows if x["type"] == "F" and not (int(x["flags"]) & 3)]
    marks = [x for x in rows if x["type"] == "M"]
    by_gen = defaultdict(list)
    for f in frames:
        by_gen[f["arg"]].append(int(f["produce_us"]))
    med = {g: statistics.median(v) for g, v in by_gen.items()}
    # provoke intervals
    intervals = []
    open_ = {}
    for m in sorted(marks, key=lambda x: int(x["t_us"])):
        if m["kind"] != "provoke":
            continue
        code = int(m["arg"])
        if m["phase"] == "1":
            open_[code] = int(m["t_us"])
        elif m["phase"] == "2" and code in open_:
            intervals.append((code, open_.pop(code), int(m["t_us"])))
    sdw = [(int(m["t_us"]) - int(m["lateness_us"] or 0), int(m["t_us"]), int(m["lateness_us"] or 0), int(m["arg"]))
           for m in marks if m["kind"] == "sd_write"]

    out = ["| condition | writes | write med/max ms | MB/s | frames in window | upscale med/max ms | anomalies (>=3x gen median) | anomaly rate |",
           "|---|---|---|---|---|---|---|---|"]
    agg = defaultdict(lambda: {"frames": [], "writes": [], "anom": 0, "bytes": 0, "dur": 0})
    for code, t0, t1 in intervals:
        mode, chunk = code >> 24, code & 0xFFFFFF
        key = (mode, chunk)
        fr = [f for f in frames if t0 <= int(f["t_us"]) <= t1 + 200000]
        ws = [w for w in sdw if t0 <= w[1] <= t1]
        a_ = agg[key]
        a_["frames"] += fr
        a_["writes"] += ws
        a_["anom"] += sum(1 for f in fr if int(f["produce_us"]) >= 3 * max(med.get(f["arg"], 0), 1000))
        a_["bytes"] += sum(w[3] for w in ws)
        a_["dur"] += (t1 - t0)
    # baseline: frames outside all intervals
    inside = lambda t: any(t0 - 200000 <= t <= t1 + 400000 for _, t0, t1 in intervals)
    base = [f for f in frames if not inside(int(f["t_us"]))]
    base_anom = sum(1 for f in base if int(f["produce_us"]) >= 3 * max(med.get(f["arg"], 0), 1000))
    for (mode, chunk), a_ in sorted(agg.items()):
        ups = sorted(int(f["upscale_us"]) for f in a_["frames"])
        wd = sorted(w[2] for w in a_["writes"])
        mbps = (a_["bytes"] / 1e6) / (a_["dur"] / 1e6) if a_["dur"] else 0
        out.append(f"| {NAMES[mode]} {chunk} B | {len(wd)} | {statistics.median(wd)/1000 if wd else 0:.1f} / {max(wd)/1000 if wd else 0:.0f} | {mbps:.2f} | {len(ups)} | "
                   f"{statistics.median(ups)/1000 if ups else 0:.1f} / {max(ups)/1000 if ups else 0:.0f} | {a_['anom']} | {100*a_['anom']/max(len(ups),1):.1f}% |")
    ups = sorted(int(f["upscale_us"]) for f in base)
    out.append(f"| (baseline, outside provocations) | | | | {len(ups)} | {statistics.median(ups)/1000 if ups else 0:.1f} / {max(ups)/1000 if ups else 0:.0f} | {base_anom} | {100*base_anom/max(len(ups),1):.1f}% |")
    text = "\n".join(out)
    print(text)
    (run_dir / "sd_experiment.md").write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
