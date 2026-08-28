#!/usr/bin/env python3
"""compare_runs.py -- A/B table for two runs (jitter work stream).

    python host/jitter-lab/compare_runs.py RUN-20260828-06 RUN-20260828-07

Requires analyze.py to have produced summary.json in both run dirs. Prints a
markdown table: stall rate, producer anomalies, SD write/read duration and
count, share of wall time inside SD writes.
"""
import csv
import glob
import json
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load(run):
    rows = []
    for fp in [HERE / "runs" / run / "frames.csv"] + sorted(glob.glob(str(HERE / "runs" / run / "frames.e*.csv"))):
        if Path(fp).exists():
            rows += list(csv.DictReader(open(fp, encoding="utf-8")))
    seen = {}
    for r in rows:
        seen[(r["seq"], r["t_us"])] = r
    return list(seen.values())


def metrics(run):
    rows = load(run)
    fr = [r for r in rows if r["type"] == "F"]
    sp = [r for r in rows if r["type"] == "M"]
    eps = {}
    for r in fr:
        eps.setdefault(int(r["t_us"]) // 10**11, []).append(int(r["t_us"]))
    span = sum((max(v) - min(v)) / 1e6 for v in eps.values())
    s = json.load(open(HERE / "runs" / run / "summary.json", encoding="utf-8"))
    rep = (HERE / "runs" / run / "report.md").read_text(encoding="utf-8")
    anomalies = None
    for line in rep.splitlines():
        if line.startswith("- count ") and "valid frames" in line:
            anomalies = int(line.split()[2])
            break
    d = {"span_h": span / 3600, "frames": len(fr), "stalls": s["stalls"], "stall_rate_h": s["stalls"] / (span / 3600) if span else 0,
         "warns": s["warns"], "overrun": s["overrun"], "anomalies": anomalies,
         "p99_ms": s["lateness_p99_us"] / 1000, "max_ms": s["lateness_max_us"] / 1000}
    for k in ("sd_write", "sd_read"):
        dd = sorted(int(r["lateness_us"] or 0) for r in sp if r["kind"] == k)
        if dd:
            d[k] = {"n": len(dd), "med_ms": statistics.median(dd) / 1000, "p90_ms": dd[int(.9 * len(dd)) - 1] / 1000,
                    "p99_ms": dd[int(.99 * len(dd)) - 1] / 1000, "max_ms": dd[-1] / 1000,
                    "pct": 100 * sum(dd) / 1e6 / span if span else 0}
    return d


def main():
    a, b = sys.argv[1], sys.argv[2]
    A, B = metrics(a), metrics(b)
    rows = [
        ("span (h)", f"{A['span_h']:.2f}", f"{B['span_h']:.2f}"),
        ("frames", A["frames"], B["frames"]),
        ("in-scope stalls (>=100 ms)", A["stalls"], B["stalls"]),
        ("stall rate / h", f"{A['stall_rate_h']:.1f}", f"{B['stall_rate_h']:.1f}"),
        ("warns (50-100 ms)", A["warns"], B["warns"]),
        ("producer anomalies (>=3x median)", A["anomalies"], B["anomalies"]),
        ("overrun frames (out of scope)", A["overrun"], B["overrun"]),
        ("lateness p99 / max (ms)", f"{A['p99_ms']:.0f} / {A['max_ms']:.0f}", f"{B['p99_ms']:.0f} / {B['max_ms']:.0f}"),
    ]
    for k in ("sd_write", "sd_read"):
        if k in A and k in B:
            rows += [
                (f"{k} count", A[k]["n"], B[k]["n"]),
                (f"{k} median / p90 / p99 (ms)", f"{A[k]['med_ms']:.2f} / {A[k]['p90_ms']:.1f} / {A[k]['p99_ms']:.1f}", f"{B[k]['med_ms']:.2f} / {B[k]['p90_ms']:.1f} / {B[k]['p99_ms']:.1f}"),
                (f"{k} max (ms)", f"{A[k]['max_ms']:.0f}", f"{B[k]['max_ms']:.0f}"),
                (f"time inside {k} (% of span)", f"{A[k]['pct']:.1f}", f"{B[k]['pct']:.1f}"),
            ]
    print(f"| metric | {a} | {b} |\n|---|---|---|")
    for name, x, y in rows:
        print(f"| {name} | {x} | {y} |")


if __name__ == "__main__":
    main()
