#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""soak.py -- a soak run against the dev unit on p3a 2.x: set the workload,
log the UART, pull the frame trace, then analyze (docs/2.0/TESTING.md, T5).

    python host/p3a-lab/lab.py flash trace          # a build that traces; keeps the unit's Wi-Fi
    python host/jitter-lab/soak.py --hours 3 [--mix work|stress64] [--note "..."]

It replaces soak.ps1, stress.py's set-up and snapshot_settings.py, which
speak 1.x's API. The pieces that already speak 2.x do the work:
serial_logger.py (UART, without resetting the board), pull_frames.py (the
frame ring over HTTP), analyze.py and run_audit.py (the report).

What it does, in order:
  1. Refuses any device that is not the dev unit (hostname p3a-fab) or
     whose build does not trace.
  2. Saves the unit's settings and active playset into the run's folder.
  3. Sets the workload: the API keys from the lab's secrets file (never
     printed, never written to the run's folder), a 30 s dwell, and the
     mix's playset.
  4. Starts the logger and the puller, waits the hours, stops them.
  5. Runs the analysis into runs/<RUN>/report.md and prints its verdict.
  6. Restores the playset and the settings it changed.

The mixes:
  work      "the normal workload" of the jitter pass bar: one channel of
            each kind of source, playing and downloading at once. 1.x's
            runs used a playset of Fab's named "Work mix" whose contents
            were not recorded; this is its stated intent, written down.
  stress64  64 channels across every source: the playset shape that
            livelocked 1.x's SD card under memory pressure. For the memory
            soak, not for the jitter pass bar.

Run data stays in host/jitter-lab/runs/<RUN>/ (not tracked).
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
import time
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "p3a-lab"))
import lab  # noqa: E402

EXPECTED_HOSTNAME = "p3a-fab"
# The key's name in the lab's secrets file -> the setting it goes into.
KEY_SETTINGS = {"giphy_api_key": "giphy_api_key", "klipy_api_key": "klipy_api_key", "ham_api_key": "harvard_api_key",
                "si_api_key": "smithsonian_api_key"}


def channel(source, kind="", identifier="", weight=100):
    return {"source": source, "kind": kind, "identifier": identifier, "display_name": "", "weight": weight, "offset": 0}


def mix_work():
    return {"name": "", "channels": [
        channel("local"),
        channel("makapix", "promoted"),
        channel("giphy", "trending"),
        channel("giphy", "search", "pixel art"),
        channel("klipy", "gif:trending"),
        channel("klipy", "sticker:trending"),
        channel("museum", "vam:category", "THES48966"),      # V&A drawings
        channel("museum", "cma:department", "Japanese Art"),
    ]}


def mix_stress64():
    words = ["cats", "dogs", "space", "ocean", "forest", "city", "rain", "fire", "robot", "pixel art", "retro", "neon",
             "mountain", "desert", "snow", "flowers", "birds", "fish", "cars", "trains"]
    channels = [channel("local"), channel("makapix", "promoted"), channel("giphy", "trending"),
                channel("klipy", "gif:trending"), channel("klipy", "sticker:trending")]
    channels += [channel("giphy", "search", w) for w in words]
    channels += [channel("klipy", "gif:search", w) for w in words]
    channels += [channel("klipy", "sticker:search", w) for w in words[:12]]
    for kind, term in (("vam:category", "THES48966"), ("vam:category", "THES48903"), ("cma:department", "Japanese Art"),
                       ("cma:department", "Prints"), ("cma:type", "Painting"), ("cma:type", "Drawing")):
        channels.append(channel("museum", kind, term))
    return {"name": "", "channels": channels[:64]}


MIXES = {"work": mix_work, "stress64": mix_stress64}


class Device:
    def __init__(self, host: str):
        self.host = host.rstrip("/")

    def call(self, method: str, path: str, body=None, timeout: float = 15.0):
        r = requests.request(method, self.host + path, json=body, timeout=timeout)
        doc = r.json()
        if not doc.get("ok"):
            raise SystemExit(f"{method} {path}: {doc.get('code')}: {doc.get('error')}")
        return doc["data"]


def new_run_id() -> str:
    day = dt.date.today().strftime("%Y%m%d")
    n = 1
    while (HERE / "runs" / f"RUN-{day}-{n:02d}").exists():
        n += 1
    return f"RUN-{day}-{n:02d}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--mix", choices=sorted(MIXES), default="work")
    ap.add_argument("--host", default=f"http://{EXPECTED_HOSTNAME}.local")
    ap.add_argument("--every", type=float, default=120.0, help="seconds between pulls of the frame ring")
    ap.add_argument("--note", default="")
    ap.add_argument("--keep", action="store_true", help="leave the workload on the unit afterwards")
    a = ap.parse_args()
    dev = Device(a.host)

    # Just flashed, the unit takes a while to join the network and announce its name.
    for attempt in range(12):
        try:
            status = dev.call("GET", "/api/v2/status")
            break
        except requests.exceptions.ConnectionError:
            if attempt == 11:
                raise
            time.sleep(10)
    if status["network"]["hostname"] != EXPECTED_HOSTNAME:
        raise SystemExit(f"REFUSING: {a.host} is {status['network']['hostname']!r}, not {EXPECTED_HOSTNAME!r}")
    probe = requests.get(dev.host + "/api/v2/debug/frames/stats", timeout=15)
    if probe.status_code != 200:
        raise SystemExit("this build does not trace: flash one that does (python host/p3a-lab/lab.py flash trace)")
    if requests.get(dev.host + "/api/v2/test/log", timeout=15).status_code == 200:
        print("note: this build has the test hooks; the pass bar is measured on a build that only adds the trace")

    run = new_run_id()
    run_dir = HERE / "runs" / run
    run_dir.mkdir(parents=True)
    port = lab.find_port()
    before_settings = dev.call("GET", "/api/v2/settings")
    before_playset = dev.call("GET", "/api/v2/player/playset")
    # The saved settings carry masked keys only: nothing secret reaches the run's folder.
    (run_dir / "before.json").write_text(json.dumps({"settings": before_settings, "playset": before_playset}, indent=1),
                                         encoding="utf-8")
    keys = lab.load_secrets().get("api_keys", {})
    wanted = {setting: keys[name] for name, setting in KEY_SETTINGS.items() if keys.get(name)}
    changed = dev.call("PATCH", "/api/v2/settings", {**wanted, "dwell_s": 30})["changed"]
    playset = MIXES[a.mix]()
    dev.call("PUT", "/api/v2/player/playset", playset)
    meta = {"run": run, "started": dt.datetime.now().isoformat(timespec="seconds"), "hours": a.hours, "mix": a.mix,
            "channels": len(playset["channels"]), "note": a.note, "firmware": status["version"], "built": status["built"],
            "keys_set": sorted(wanted), "settings_changed": [c for c in changed if not c.endswith("_api_key")]}
    (run_dir / "run.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    print(f"{run}: {a.mix} mix, {len(playset['channels'])} channels, {a.hours} h, firmware {status['version']}", flush=True)

    dev.call("POST", "/api/v2/debug/frames/reset")
    logger = subprocess.Popen([sys.executable, str(HERE / "serial_logger.py"), port, run],
                              stdout=(run_dir / "logger.stdout.log").open("w"), stderr=subprocess.STDOUT)
    deadline = time.monotonic() + a.hours * 3600
    try:
        while time.monotonic() < deadline:
            left_h = (deadline - time.monotonic()) / 3600
            # The puller ends by itself; if it dies (the unit restarted, the laptop slept), it is started again.
            puller = subprocess.run([sys.executable, str(HERE / "pull_frames.py"), run, "--host", dev.host, "--every",
                                     str(a.every), "--from-head", "--hours", f"{max(left_h, 0.01):.4f}"],
                                    stdout=(run_dir / "puller.stdout.log").open("a"), stderr=subprocess.STDOUT)
            if puller.returncode == 0:
                break
            time.sleep(10)
    finally:
        subprocess.run([sys.executable, str(HERE / "pull_frames.py"), run, "--host", dev.host, "--once"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        logger.terminate()
        try:
            logger.wait(timeout=10)
        except subprocess.TimeoutExpired:
            logger.kill()
        if not a.keep:
            try:
                dev.call("PUT", "/api/v2/player/playset", before_playset)
                dev.call("PATCH", "/api/v2/settings", {"dwell_s": before_settings["dwell_s"]})
            except (SystemExit, requests.RequestException) as why:
                print(f"could not restore the unit: {why}")

    for tool in ("analyze.py", "run_audit.py"):
        out = subprocess.run([sys.executable, str(HERE / tool), run], capture_output=True, text=True)
        (run_dir / f"{tool[:-3]}.stdout.log").write_text(out.stdout + out.stderr, encoding="utf-8")
        print(f"--- {tool}\n{out.stdout[-1500:]}")
    print(f"{run}: report at {run_dir / 'report.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
