#!/usr/bin/env python3
"""run_audit.py -- reboots and coverage gaps of a soak run (jitter work stream).

    python host/jitter-lab/run_audit.py RUN-20260902-01 [--gap-min 5]

A soak must survive accidental device resets (a bumped USB-C cable) and laptop
sleeps without those being mistaken for firmware faults. This script reads
runs/<RUN>/:
    boots.jsonl   reboots the puller detected (ring sequence restarted)
    uart.log      host-timestamped UART lines: ROM reset reason lines
                  ("rst:0x1 (POWERON)"), panic / watchdog / brownout markers,
                  deliberate reboots ("Restarting" / "esp_restart")
    stats.jsonl   puller heartbeats -> coverage gaps (laptop asleep, device
                  unreachable): ring rows during a gap longer than the ring
                  depth are lost, so the run's frame coverage excludes them
and prints a markdown audit: one row per reset with its classification
(external = power/cable, firmware = panic/watchdog, deliberate = software
reboot request, unknown) plus the coverage-gap table and the covered hours.
"""
import argparse
import datetime as dt
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent

RST_RE = re.compile(r"^(\S+) rst:(0x[0-9a-f]+) \(([A-Z0-9_]+)\)")
EXTERNAL = {"POWERON", "BROWNOUT", "RTC_BROWN_OUT", "RTC_BROWN_OUT_RESET", "EXT_SYS", "CHIP_POWERON",
            "USB_UART_CHIP_RESET", "JTAG_CPU_RESET", "USB_UART_HPSYS", "USB_JTAG_HPSYS"}
WATCHDOG = {"TG0WDT_SYS_RST", "TG1WDT_SYS_RST", "RTCWDT_RTC_RESET", "TG0WDT_CPU_RESET", "TG1WDT_CPU_RESET",
            "RTCWDT_CPU_RESET", "RTCWDT_SYS_RST", "SUPER_WDT_RESET", "RTCWDT_BROWN_OUT_RESET", "TG0WDT_HPSYS", "TG1WDT_HPSYS"}
PANIC_RE = re.compile(r"Guru Meditation|abort\(\) was called|Task watchdog got triggered|Brownout detector|assert failed|Stack smashing|Interrupt wdt timeout")
DELIBERATE_RE = re.compile(r"Restarting\.\.\.|esp_restart|Rebooting|reboot requested|/action/reboot|MSC.*reboot", re.I)


def parse_ts(s):
    try:
        return dt.datetime.fromisoformat(s)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--gap-min", type=float, default=5.0, help="heartbeat gap (minutes) that counts as a coverage gap")
    ap.add_argument("--window-s", type=float, default=90.0, help="look-back window before a reset for panic/deliberate markers")
    a = ap.parse_args()
    run_dir = HERE / "runs" / a.run
    out = [f"# Run audit: {a.run}", ""]

    # ---- resets from UART
    resets, markers = [], []
    uart = run_dir / "uart.log"
    if uart.exists():
        with uart.open(encoding="utf-8", errors="replace") as f:
            for line in f:
                m = RST_RE.match(line)
                if m:
                    resets.append((parse_ts(m.group(1)), m.group(2), m.group(3)))
                    continue
                ts = parse_ts(line[:23])
                if ts is None:
                    continue
                if PANIC_RE.search(line):
                    markers.append((ts, "panic", line.strip()[:160]))
                elif DELIBERATE_RE.search(line):
                    markers.append((ts, "deliberate", line.strip()[:160]))
    boots = []
    bj = run_dir / "boots.jsonl"
    if bj.exists():
        boots = [json.loads(l) for l in bj.read_text(encoding="utf-8").splitlines() if l.strip()]

    # Serial-link drops and boot banners: when the USB link itself goes away
    # (cable / power event) the CH343 bridge re-enumerates, the logger sees
    # "Access is denied" and the ROM "rst:" line is lost. The app's
    # "cpu_start: Pro cpu start user code" line right after the reopen is
    # then the only boot evidence.
    link_drops, boot_banners = [], []
    if uart.exists():
        with uart.open(encoding="utf-8", errors="replace") as f:
            for line in f:
                ts = parse_ts(line[:23])
                if ts is None:
                    continue
                if "### serial error" in line:
                    link_drops.append((ts, line.strip()[24:140]))
                elif "cpu_start: Pro cpu start user code" in line:
                    boot_banners.append(ts)
    out += [f"UART reset lines: {len(resets)}; puller-detected reboots (epochs): {len(boots)}; "
            f"boot banners: {len(boot_banners)}; serial link drops: {len(link_drops)}", ""]
    # Every puller epoch is a reboot. The puller notices up to one poll period
    # late, so look back up to 150 s for the evidence: a captured ROM reset
    # line (classified below), else a serial-link drop (external), else a
    # panic / deliberate marker, else unknown.
    if boots:
        out += ["Reboots (puller epochs) and their evidence:", "",
                "| epoch | detected (host) | class | evidence |", "|---|---|---|---|"]
        for b in boots:
            bt = parse_ts(b.get("host_ts"))
            if bt is None:
                continue
            rst = [r for r in resets if r[0] and 0 <= (bt - r[0]).total_seconds() <= 150]
            drop = [d for d in link_drops if 0 <= (bt - d[0]).total_seconds() <= 150]
            mk = [m for m in markers if m[0] <= bt and (bt - m[0]).total_seconds() <= 150 + a.window_s]
            if rst:
                cls, ev = "see reset table", f"rst {rst[-1][1]} ({rst[-1][2]}) at {rst[-1][0].isoformat(timespec='seconds')}"
            elif mk and mk[-1][1] == "panic":
                cls, ev = "firmware (panic)", mk[-1][2]
            elif mk and mk[-1][1] == "deliberate":
                cls, ev = "deliberate (software reboot)", mk[-1][2]
            elif drop:
                cls, ev = "external (USB link dropped, ROM reset line lost)", f"{drop[-1][0].isoformat(timespec='seconds')} {drop[-1][1]}"
            else:
                cls, ev = "unknown (no reset line, no link drop, no marker)", ""
            out.append(f"| {b.get('epoch')} | {bt.isoformat(timespec='seconds')} | {cls} | {ev} |")
        out += [""]
    out += ["| # | host time | rst | reason | class | evidence (last marker before reset) |", "|---|---|---|---|---|---|"]
    classes = {}
    for i, (ts, code, reason) in enumerate(resets, 1):
        ev = ""
        cls = "unknown"
        if reason in EXTERNAL:
            cls = "external (power/cable)"
        elif reason in WATCHDOG:
            cls = "firmware (watchdog)"
        elif reason.startswith("SW_"):
            cls = "software reset: unclassified"
        if ts is not None:
            win = [m for m in markers if m[0] <= ts and (ts - m[0]).total_seconds() <= a.window_s]
            if win:
                last = win[-1]
                ev = f"{last[1]}: {last[2]}"
                if reason.startswith("SW_") or reason in WATCHDOG:
                    if last[1] == "panic":
                        cls = "firmware (panic)"
                    elif last[1] == "deliberate":
                        cls = "deliberate (software reboot)"
        classes[cls] = classes.get(cls, 0) + 1
        when = ts.isoformat(timespec="seconds") if ts else "?"
        out.append(f"| {i} | {when} | {code} | {reason} | {cls} | {ev} |")
    if not resets:
        out.append("| (none) | | | | | |")
    out += ["", "Classes: " + (", ".join(f"{k} x{v}" for k, v in classes.items()) if classes else "no resets"), ""]
    if boots:
        out += ["Puller epochs:"]
        out += [f"- {b.get('host_ts')} epoch {b.get('epoch')} (device head seq {b.get('device_head')})" for b in boots]
        out += [""]

    # ---- coverage gaps from puller heartbeats
    stats = run_dir / "stats.jsonl"
    hb = []
    if stats.exists():
        for l in stats.read_text(encoding="utf-8").splitlines():
            try:
                hb.append(parse_ts(json.loads(l)["host_ts"]))
            except Exception:
                pass
    hb = [h for h in hb if h]
    gaps, covered = [], 0.0
    for p, q in zip(hb, hb[1:]):
        d = (q - p).total_seconds()
        if d > a.gap_min * 60:
            gaps.append((p, q, d))
        else:
            covered += d
    span = ""
    if hb:
        span = f" ({hb[0].isoformat(timespec='seconds')} .. {hb[-1].isoformat(timespec='seconds')})"
    out += [f"Puller heartbeats: {len(hb)}{span}",
            f"Covered time (heartbeat spans <= {a.gap_min:g} min): {covered/3600:.2f} h; coverage gaps: {len(gaps)}", ""]
    if gaps:
        out += ["| gap start | gap end | minutes | note |", "|---|---|---|---|"]
        for p, q, d in gaps:
            inside = any((parse_ts(b.get("host_ts")) or p) >= p and (parse_ts(b.get("host_ts")) or p) <= q for b in boots)
            note = "device rebooted inside" if inside else "laptop asleep / device unreachable"
            out.append(f"| {p.isoformat(timespec='seconds')} | {q.isoformat(timespec='seconds')} | {d/60:.1f} | {note} |")
        out.append("")
    text = "\n".join(out)
    print(text)
    (run_dir / "audit.md").write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
