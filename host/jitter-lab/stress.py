#!/usr/bin/env python3
"""stress.py -- stress phases against the dev device while it plays (jitter work stream, Phase 4/5).

    python host/jitter-lab/stress.py RUN-ID [--host http://p3a-fab.local] [--phases swaps,playsets,polling,ota,refresh,upload,all]

Each phase is bracketed by POST /api/debug/mark (arg = phase code) so the
analyzer can attribute anomalies/stalls; the frame ring is pulled after each
phase. Settings changed here (playset) are restored at the end. Phases:
  swaps     rapid swap_next/swap_back (60 in 60 s)
  playsets  switch between the user's playsets and back (6 switches)
  polling   web-UI style polling storm: /api/state, /api/memory, /config, /channels/stats x 10/s for 60 s
  ota       POST /ota/check x3 (25 s apart)
  refresh   PUT /settings/refresh_override toggle + re-execute playset (forces channel refreshes)
  upload    POST /upload of the 30 fps bar GIF x3 (SD writes + swap)
Reports per phase: frames, upscale med/max, producer anomalies, consumer-late frames
(lateness >= 100 ms with ready_margin >= 0), and warns/hard from the device stats.
"""
import argparse
import csv
import json
import statistics
import subprocess
import sys
import time
import urllib.parse
from collections import defaultdict
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
CODES = {"swaps": 0xA1, "playsets": 0xA2, "polling": 0xA3, "ota": 0xA4, "refresh": 0xA5, "upload": 0xA6}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--phases", default="all")
    ap.add_argument("--gap", type=float, default=15.0)
    a = ap.parse_args()
    H = a.host
    hn = requests.get(f"{H}/api/device-name", timeout=10).json().get("hostname")
    if hn != "p3a-fab":
        raise SystemExit(f"REFUSING: {H} is {hn!r}, not p3a-fab")
    phases = list(CODES) if a.phases == "all" else a.phases.split(",")
    run_dir = HERE / "runs" / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    head = requests.get(f"{H}/api/debug/frames/stats", timeout=15).json()["data"]["next_seq"]
    (run_dir / "pull_state.json").write_text(json.dumps({"next_seq": int(head), "rows": 0, "epoch": 0}), encoding="utf-8")
    log = (run_dir / "stress.log").open("a", encoding="utf-8")

    def mark(arg):
        requests.post(f"{H}/api/debug/mark", params={"arg": arg}, timeout=10)

    def pull():
        subprocess.run([sys.executable, str(HERE / "pull_frames.py"), a.run, "--host", H, "--once"], check=False, stdout=subprocess.DEVNULL)

    def stats():
        d = requests.get(f"{H}/api/debug/frames/stats", timeout=15).json()["data"]
        return d["stalls_hard"] - d.get("stalls_overrun", 0), d["stalls_warn"], d["lateness_max_us"] // 1000

    active = requests.get(f"{H}/playsets/active", timeout=15).json()["data"].get("active_playset") or {}
    active_name = active.get("name")
    playsets = [p.get("name") for p in requests.get(f"{H}/playsets", timeout=15).json().get("data", {}).get("playsets", []) if isinstance(p, dict)]
    print(f"active playset: {active_name!r}; playsets: {playsets}", flush=True)

    results = {}
    for ph in phases:
        code = CODES[ph]
        before = stats()
        mark(code * 2)          # begin marker = 2*code (even)
        t0 = time.time()
        try:
            if ph == "swaps":
                for i in range(60):
                    requests.post(f"{H}/action/{'swap_next' if i % 3 else 'swap_back'}", timeout=10)
                    time.sleep(1.0)
            elif ph == "playsets":
                others = [p for p in playsets if p and p != active_name][:2] or [active_name]
                for i in range(6):
                    name = others[i % len(others)] if i % 2 == 0 else active_name
                    if name:
                        requests.post(f"{H}/playset/{urllib.parse.quote(name)}", timeout=30)
                    time.sleep(10)
            elif ph == "polling":
                end = time.time() + 60
                eps = ["/api/state", "/api/memory?silent", "/config", "/channels/stats", "/playsets/active"]
                i = 0
                while time.time() < end:
                    try:
                        requests.get(f"{H}{eps[i % len(eps)]}", timeout=5)
                    except Exception:
                        pass
                    i += 1
                    time.sleep(0.1)
            elif ph == "ota":
                for i in range(3):
                    requests.post(f"{H}/ota/check", timeout=60)
                    time.sleep(25)
            elif ph == "refresh":
                for i in range(2):
                    requests.put(f"{H}/settings/refresh_override", json={"refresh_allow_override": True}, timeout=15)
                    if active_name:
                        requests.post(f"{H}/playset/{urllib.parse.quote(active_name)}", timeout=30)
                    time.sleep(30)
                requests.put(f"{H}/settings/refresh_override", json={"refresh_allow_override": False}, timeout=15)
            elif ph == "upload":
                gif = HERE / "runs" / "test-anims" / "bar_30fps.gif"
                for i in range(3):
                    with gif.open("rb") as f:
                        requests.post(f"{H}/upload", files={"file": ("bar_30fps.gif", f, "image/gif")}, timeout=120)
                    time.sleep(15)
        except Exception as e:  # noqa: BLE001
            print(f"phase {ph}: error {e}", flush=True)
        dur = time.time() - t0
        time.sleep(a.gap)
        mark(code * 2 + 1)      # end marker = 2*code+1 (odd)
        after = stats()
        pull()
        results[ph] = {"dur_s": round(dur, 1), "new_nonoverrun_stalls": after[0] - before[0], "new_warns": after[1] - before[1], "max_ms_now": after[2]}
        line = f"phase {ph}: {results[ph]}"
        print(line, flush=True); log.write(line + "\n"); log.flush()

    # restore playback to the original playset
    if active_name:
        requests.post(f"{H}/playset/{urllib.parse.quote(active_name)}", timeout=30)

    # ---- per-phase frame analysis from the ring
    rows = list(csv.DictReader((run_dir / "frames.csv").open(encoding="utf-8")))
    rows = list({(x["seq"], x["t_us"]): x for x in rows}.values())
    frames = [x for x in rows if x["type"] == "F" and not (int(x["flags"]) & 3)]
    marks = sorted([x for x in rows if x["type"] == "M" and x["kind"] == "user"], key=lambda x: int(x["t_us"]))
    by_gen = defaultdict(list)
    for f in frames:
        by_gen[f["arg"]].append(int(f["produce_us"]))
    med = {g: statistics.median(v) for g, v in by_gen.items()}
    spans = {}
    for m in marks:
        arg = int(m["arg"])
        for ph, code in CODES.items():
            if arg == code * 2:
                spans.setdefault(ph, [None, None])[0] = int(m["t_us"])
            elif arg == code * 2 + 1:
                spans.setdefault(ph, [None, None])[1] = int(m["t_us"])
    out = ["| phase | dur s | frames | upscale med / max ms | producer anomalies | consumer-late (>=100 ms, ready in time) | new non-overrun stalls (device) |", "|---|---|---|---|---|---|---|"]
    for ph in phases:
        t0, t1 = spans.get(ph, [None, None])
        r = results.get(ph, {})
        if t0 is None or t1 is None:
            out.append(f"| {ph} | {r.get('dur_s')} | ? | | | | {r.get('new_nonoverrun_stalls')} |"); continue
        fr = [f for f in frames if t0 <= int(f["t_us"]) <= t1]
        ups = sorted(int(f["upscale_us"]) for f in fr)
        anom = sum(1 for f in fr if int(f["produce_us"]) >= 3 * max(med.get(f["arg"], 0), 1000))
        clate = sum(1 for f in fr if int(f["lateness_us"]) >= 100000 and int(f["ready_margin_us"]) >= 0)
        out.append(f"| {ph} | {r.get('dur_s')} | {len(fr)} | {statistics.median(ups)/1000 if ups else 0:.1f} / {max(ups)/1000 if ups else 0:.0f} | {anom} | {clate} | {r.get('new_nonoverrun_stalls')} |")
    text = "\n".join(out)
    print(text)
    (run_dir / "stress.md").write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
