#!/usr/bin/env python3
"""serial_logger.py -- persistent, reset-free UART logger for the jitter work stream.

    python host/jitter-lab/serial_logger.py COM5 RUN-20260828-01 [--baud 115200]

Writes into host/jitter-lab/runs/<RUN>/:
    uart.log       every line, prefixed with host ISO timestamp
    stalls.jsonl   one JSON object per JTR| stall report (parsed)
    state.json     heartbeat: pid, started, last_line, lines, stall_reports, errors

Opening the port: DTR and RTS are forced low BEFORE open() so the CH343 bridge
does not pulse the ESP32-P4 reset line (verify: /api/state uptime must keep
climbing across a logger start; see docs/jitter/LOG.md).
"""
import argparse
import json
import os
import sys
import time
import datetime as dt
from pathlib import Path

import serial  # pyserial

HERE = Path(__file__).resolve().parent


def now_iso():
    return dt.datetime.now().isoformat(timespec="milliseconds")


class JtrParser:
    """Collects JTR| lines between JTR|STALL and JTR|END into one record."""

    def __init__(self, out_path: Path):
        self.out = out_path
        self.cur = None

    def feed(self, line: str, host_ts: str):
        if not line.startswith("JTR|"):
            return
        body = line[4:]
        if body.startswith("STALL "):
            self.cur = {"host_ts": host_ts, "header": body[6:], "frames": [], "marks": [], "tasks": [], "raw": [line]}
            return
        if self.cur is None:
            return
        self.cur["raw"].append(line)
        if body.startswith("F "):
            self.cur["frames"].append(body[2:])
        elif body.startswith("M "):
            self.cur["marks"].append(body[2:])
        elif body.startswith("T "):
            self.cur["tasks"].append(body[2:])
        elif body.startswith("END"):
            rec = self.cur
            self.cur = None
            with self.out.open("a", encoding="utf-8") as f:
                f.write(json.dumps(rec) + "\n")
            return True
        return False


def open_port(port: str, baud: int) -> serial.Serial:
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 1.0
    s.dtr = False
    s.rts = False
    s.open()
    # Belt and braces: re-assert after open (some drivers apply defaults at open).
    s.dtr = False
    s.rts = False
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("run")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--runs-dir", default=str(HERE / "runs"))
    a = ap.parse_args()

    run_dir = Path(a.runs_dir) / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    uart_log = run_dir / "uart.log"
    state_path = run_dir / "state.json"
    parser = JtrParser(run_dir / "stalls.jsonl")

    state = {
        "pid": os.getpid(), "port": a.port, "baud": a.baud, "run": a.run,
        "started": now_iso(), "last_line": None, "lines": 0, "stall_reports": 0,
        "errors": 0, "reopens": 0, "status": "starting",
    }

    def save_state():
        tmp = state_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
        os.replace(tmp, state_path)

    save_state()
    ser = None
    last_state_save = 0.0
    buf = b""
    while True:
        try:
            if ser is None:
                ser = open_port(a.port, a.baud)
                state["status"] = "open"
                state["reopens"] += 1
                save_state()
                with uart_log.open("a", encoding="utf-8") as f:
                    f.write(f"{now_iso()} ### logger opened {a.port} @ {a.baud} (reopen #{state['reopens']})\n")
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    ts = now_iso()
                    with uart_log.open("a", encoding="utf-8") as f:
                        f.write(f"{ts} {line}\n")
                    state["lines"] += 1
                    state["last_line"] = ts
                    if parser.feed(line, ts):
                        state["stall_reports"] += 1
                        state["last_stall_report"] = ts
                        save_state()
            if time.time() - last_state_save > 5:
                save_state()
                last_state_save = time.time()
        except serial.SerialException as e:
            state["errors"] += 1
            state["status"] = f"error: {e}"
            save_state()
            with uart_log.open("a", encoding="utf-8") as f:
                f.write(f"{now_iso()} ### serial error: {e}\n")
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(3)
        except KeyboardInterrupt:
            state["status"] = "stopped"
            save_state()
            return 0


if __name__ == "__main__":
    sys.exit(main())
