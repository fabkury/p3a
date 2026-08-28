#!/usr/bin/env python3
"""analyze.py -- turn a run's frames.csv (+ stalls.jsonl) into report.md (jitter work stream).

    python host/jitter-lab/analyze.py RUN-20260828-01 [--stall-ms 100] [--warn-ms 50] [--window-s 2.0]

Classification per frame (flags from frame_trace.h):
    excluded   BASELINED or MAX_SPEED (lateness undefined by design)
    overrun    ready_margin_us < 0, the producer lateness explains the frame
               lateness (|margin| >= lateness - 1 vsync), AND produce_us is in
               line with the artwork's (generation's) median producer time
               -> out of scope (slow decode). An anomalous produce_us (>= 3x the
               generation median, or >= 1 frame duration above it) means the
               producer was STARVED, which counts as a stall.
    stall      lateness >= stall_ms and not overrun         -> IN SCOPE
    warn       warn_ms <= lateness < stall_ms and not overrun

Attribution: for every stall, marks whose [begin,end] overlaps
[present - lateness - window, present] are listed; kinds are scored by how often
they co-occur vs. their base rate. Also joins UART JTR| reports (task run-time
deltas) by nearest seq.
"""
import argparse
import csv
import json
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
VSYNC_US = 16667
FLAG_MAX_SPEED, FLAG_BASELINED, FLAG_RESYNCED, FLAG_UI, FLAG_BLACK = 1, 2, 4, 8, 16


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0
    k = int(round((len(sorted_vals) - 1) * p))
    return sorted_vals[max(0, min(k, len(sorted_vals) - 1))]


def load_rows(path: Path):
    frames, marks = [], []
    with path.open(encoding="utf-8") as f:
        rd = csv.DictReader(f)
        for r in rd:
            try:
                if r["type"] == "F":
                    frames.append({
                        "seq": int(r["seq"]), "t": int(r["t_us"]), "gen": int(r["arg"] or 0),
                        "late": int(r["lateness_us"] or 0), "margin": int(r["ready_margin_us"] or 0),
                        "produce": int(r["produce_us"] or 0), "decode": int(r["decode_us"] or 0),
                        "upscale": int(r["upscale_us"] or 0), "free_wait": int(r["free_wait_us"] or 0),
                        "vsync_wait": int(r["vsync_wait_us"] or 0), "dur": int(r["duration_ms"] or 0),
                        "qd": int(r["queue_depth"] or 0), "flags": int(r["flags"] or 0),
                    })
                elif r["type"] == "M":
                    marks.append({
                        "seq": int(r["seq"]), "t": int(r["t_us"]), "kind": r["kind"], "phase": int(r["phase"] or 0),
                        "arg": int(r["arg"] or 0), "core": int(r["core"] or 0), "task": r["task_tag"],
                    })
            except (KeyError, ValueError):
                continue
    frames.sort(key=lambda x: x["seq"])
    marks.sort(key=lambda x: x["seq"])
    return frames, marks


def pair_marks(marks):
    """Turn BEGIN/END pairs into intervals; EVENTs become zero-length intervals."""
    open_by_key = {}
    intervals = []
    for m in marks:
        key = (m["kind"], m["arg"], m["task"])
        if m["phase"] == 1:
            open_by_key[key] = m
        elif m["phase"] == 2:
            b = open_by_key.pop(key, None)
            if b is None:
                # END without BEGIN (BEGIN fell off the ring or error path): treat as instant.
                intervals.append({"kind": m["kind"], "arg": m["arg"], "task": m["task"], "t0": m["t"], "t1": m["t"], "core": m["core"], "dangling": True})
            else:
                intervals.append({"kind": m["kind"], "arg": m["arg"], "task": m["task"], "t0": b["t"], "t1": m["t"], "core": m["core"], "dangling": False})
        else:
            intervals.append({"kind": m["kind"], "arg": m["arg"], "task": m["task"], "t0": m["t"], "t1": m["t"], "core": m["core"], "dangling": False})
    for b in open_by_key.values():  # still open at end of data
        intervals.append({"kind": b["kind"], "arg": b["arg"], "task": b["task"], "t0": b["t"], "t1": None, "core": b["core"], "dangling": True})
    intervals.sort(key=lambda x: x["t0"])
    return intervals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--stall-ms", type=int, default=100)
    ap.add_argument("--warn-ms", type=int, default=50)
    ap.add_argument("--window-s", type=float, default=2.0)
    ap.add_argument("--runs-dir", default=str(HERE / "runs"))
    ap.add_argument("--max-stalls", type=int, default=60, help="max stalls detailed in the report")
    a = ap.parse_args()

    run_dir = Path(a.runs_dir) / a.run
    frames_csv = run_dir / "frames.csv"
    if not frames_csv.exists():
        print(f"no frames.csv in {run_dir}")
        return 1
    frames, marks = load_rows(frames_csv)
    intervals = pair_marks(marks)
    stall_us, warn_us, window_us = a.stall_ms * 1000, a.warn_ms * 1000, int(a.window_s * 1e6)

    valid = [f for f in frames if not (f["flags"] & (FLAG_BASELINED | FLAG_MAX_SPEED))]
    lat_sorted = sorted(f["late"] for f in valid)
    # Per-generation (artwork epoch) median producer time: a frame whose producer
    # time is in line with its artwork's norm and still misses its target is a
    # decode overrun (out of scope); a frame whose producer time is anomalous
    # (>= 3x the norm or >= 1 frame duration above it) was STARVED, which is a
    # stall even though ready_margin is negative.
    by_gen = defaultdict(list)
    for f in valid:
        by_gen[f["gen"]].append(f["produce"])
    med_gen = {g: statistics.median(v) for g, v in by_gen.items()}
    overrun, stalls, warns = [], [], []
    for f in valid:
        norm = med_gen.get(f["gen"], 0)
        anomalous = f["produce"] >= max(3 * norm, norm + f["dur"] * 1000)
        is_overrun = f["margin"] < 0 and (-f["margin"] >= f["late"] - VSYNC_US) and not anomalous
        if is_overrun and f["late"] >= warn_us:
            overrun.append(f)
        elif f["late"] >= stall_us:
            stalls.append(f)
        elif f["late"] >= warn_us:
            warns.append(f)

    span_s = (frames[-1]["t"] - frames[0]["t"]) / 1e6 if len(frames) > 1 else 0.0

    # ---- attribution
    kind_base = Counter(i["kind"] for i in intervals)
    kind_hits = Counter()
    stall_details = []
    for f in stalls:
        t_present = f["t"]
        t_from = t_present - f["late"] - window_us
        hits = []
        for i in intervals:
            t1 = i["t1"] if i["t1"] is not None else t_present
            if i["t0"] <= t_present and t1 >= t_from:
                hits.append(i)
        for k in set(i["kind"] for i in hits):
            kind_hits[k] += 1
        stall_details.append((f, hits))

    # ---- UART reports join
    uart_reports = []
    sj = run_dir / "stalls.jsonl"
    if sj.exists():
        for ln in sj.read_text(encoding="utf-8").splitlines():
            try:
                uart_reports.append(json.loads(ln))
            except Exception:
                pass

    def report_for_seq(seq):
        for r in uart_reports:
            h = r.get("header", "")
            if f"seq={seq} " in h or h.startswith(f"seq={seq}"):
                return r
        return None

    # ---- write report
    out = []
    w = out.append
    w(f"# Jitter run report: {a.run}\n")
    w(f"- frames: {len(frames)} (valid for lateness: {len(valid)}), marks: {len(marks)}, span: {span_s/3600:.2f} h")
    w(f"- thresholds: warn {a.warn_ms} ms, stall {a.stall_ms} ms; attribution window {a.window_s} s before the stall's intended time")
    if lat_sorted:
        w(f"- lateness us: p50 {pct(lat_sorted,0.5)}  p90 {pct(lat_sorted,0.9)}  p99 {pct(lat_sorted,0.99)}  "
          f"p99.9 {pct(lat_sorted,0.999)}  max {lat_sorted[-1]}  mean {statistics.fmean(lat_sorted):.0f}")
    w(f"- **stalls (in scope, >= {a.stall_ms} ms): {len(stalls)}**  | warns ({a.warn_ms}-{a.stall_ms} ms): {len(warns)}  | overrun (producer late, out of scope): {len(overrun)}")
    if span_s > 0:
        w(f"- stall rate: {len(stalls) / max(span_s/3600, 1e-9):.2f} per hour")
    pass_bar = len(stalls) == 0
    w(f"- **PASS BAR (no stall >= {a.stall_ms} ms): {'PASS' if pass_bar else 'FAIL'}**\n")

    # histogram
    edges = [1000, 5000, 17000, 34000, 50000, 100000, 250000, 500000, 1000000, 2000000, 5000000]
    hist = Counter()
    for l in lat_sorted:
        b = 0
        while b < len(edges) and max(l, 0) >= edges[b]:
            b += 1
        hist[b] += 1
    w("## Lateness histogram (valid frames)\n")
    w("| bucket | frames |\n|---|---|")
    labels = ["<1ms", "1-5ms", "5-17ms", "17-34ms", "34-50ms", "50-100ms", "100-250ms", "250-500ms", "0.5-1s", "1-2s", "2-5s", ">=5s"]
    for i, lab in enumerate(labels):
        w(f"| {lab} | {hist.get(i,0)} |")
    w("")

    # producer summary
    if valid:
        prod = sorted(f["produce"] for f in valid)
        w("## Producer (decode+upscale) time, us\n")
        w(f"- p50 {pct(prod,0.5)}  p90 {pct(prod,0.9)}  p99 {pct(prod,0.99)}  max {prod[-1]}")
        qd = Counter(f["qd"] for f in valid)
        w(f"- ready-queue depth after dequeue: {dict(sorted(qd.items()))}")
        w("")

    # attribution table
    w("## Attribution (marks overlapping the stall window)\n")
    if stalls:
        w("| kind | stalls with kind in window | share of stalls | base count in run |\n|---|---|---|---|")
        for k, n in kind_hits.most_common():
            w(f"| {k} | {n} | {100.0*n/len(stalls):.0f}% | {kind_base.get(k,0)} |")
        w("")
        w("## Stall list\n")
        for f, hits in stall_details[: a.max_stalls]:
            fl = []
            if f["flags"] & FLAG_RESYNCED: fl.append("RESYNCED")
            if f["flags"] & FLAG_UI: fl.append("UI")
            if f["flags"] & FLAG_BLACK: fl.append("BLACK")
            w(f"### seq {f['seq']}  lateness {f['late']/1000:.0f} ms  margin {f['margin']/1000:.0f} ms  produce {f['produce']/1000:.1f} ms "
              f"(decode {f['decode']/1000:.1f} / upscale {f['upscale']/1000:.1f})  free_wait {f['free_wait']/1000:.1f} ms  "
              f"vsync_wait {f['vsync_wait']/1000:.1f} ms  dur {f['dur']} ms  qd {f['qd']}  gen {f['gen']}  {' '.join(fl)}")
            if hits:
                w("| kind | arg | task | core | begins (ms before present) | length ms |\n|---|---|---|---|---|---|")
                for i in sorted(hits, key=lambda x: x["t0"]):
                    ln = (i["t1"] - i["t0"]) / 1000 if i["t1"] is not None else float("nan")
                    w(f"| {i['kind']}{' (dangling)' if i['dangling'] else ''} | {i['arg']} | {i['task']} | {i['core']} | {(f['t'] - i['t0'])/1000:.1f} | {ln:.1f} |")
            else:
                w("_no marks in window_")
            rep = report_for_seq(f["seq"])
            if rep and rep.get("tasks"):
                w("\nUART run-time delta (tasks that consumed CPU in the reporter window):\n")
                w("```")
                for t in rep["tasks"][:20]:
                    w(t)
                w("```")
            w("")
        if len(stalls) > a.max_stalls:
            w(f"_... {len(stalls) - a.max_stalls} more stalls not detailed_\n")
    else:
        w("_no in-scope stalls_\n")

    if overrun:
        w("## Overrun frames (producer late; out of scope, for the record)\n")
        w(f"- count {len(overrun)}, worst lateness {max(f['late'] for f in overrun)/1000:.0f} ms, worst produce {max(f['produce'] for f in overrun)/1000:.0f} ms")
        gens = Counter(f["gen"] for f in overrun)
        w(f"- by generation (artwork epoch): {dict(gens.most_common(10))}\n")

    # mark inventory
    w("## Mark inventory\n")
    w("| kind | count | dangling |\n|---|---|---|")
    dang = Counter(i["kind"] for i in intervals if i["dangling"])
    for k, n in kind_base.most_common():
        w(f"| {k} | {n} | {dang.get(k,0)} |")
    w("")

    (run_dir / "report.md").write_text("\n".join(out), encoding="utf-8")
    summary = {
        "run": a.run, "frames": len(frames), "valid": len(valid), "span_s": span_s,
        "stalls": len(stalls), "warns": len(warns), "overrun": len(overrun), "pass": pass_bar,
        "lateness_p99_us": pct(lat_sorted, 0.99) if lat_sorted else 0,
        "lateness_max_us": lat_sorted[-1] if lat_sorted else 0,
        "attribution": dict(kind_hits.most_common()),
    }
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
