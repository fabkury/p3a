#!/usr/bin/env python3
"""snapshot_settings.py -- save/restore the dev device's runtime settings around a run.

    python host/jitter-lab/snapshot_settings.py save    RUN-20260828-01 [--host http://p3a-fab.local]
    python host/jitter-lab/snapshot_settings.py restore RUN-20260828-01
    python host/jitter-lab/snapshot_settings.py set     RUN-20260828-01 --show-fps 1 --dwell 30 --playset "Work mix"

Saved to runs/<RUN>/settings_before.json with every *_api_key field REDACTED
(GET /config returns keys in the clear). `set` records what it changed in
runs/<RUN>/settings_changed.json; `restore` puts back ONLY those fields, each
via `PUT /config?merge=true` (a PUT without merge=true REPLACES the whole
config: keys, sdcard_root and device_name are lost -- incident 2026-08-29),
dwell via PUT /settings/dwell_time, playset via POST /playset/<name>.
"""
import argparse
import json
import sys
import urllib.parse
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent

EXPECTED_HOSTNAME = "p3a-fab"   # the dev unit (Fab renamed it 2026-08-28); another p3a may answer at p3a.local


def assert_dev_unit(host: str):
    """Refuse to touch any device that is not the jitter dev unit."""
    r = requests.get(f"{host}/api/device-name", timeout=10)
    r.raise_for_status()
    hn = r.json().get("hostname")
    if hn != EXPECTED_HOSTNAME:
        raise SystemExit(f"REFUSING: {host} reports hostname {hn!r}, expected {EXPECTED_HOSTNAME!r} (wrong device?)")
RESTORE_CONFIG_FIELDS = ("show_fps", "max_speed_playback", "brightness", "rotation")


def redact(cfg: dict) -> dict:
    return {k: ("REDACTED" if k.endswith("_api_key") else v) for k, v in cfg.items()}


def get(host, path):
    r = requests.get(f"{host}{path}", timeout=15)
    r.raise_for_status()
    j = r.json()
    return j.get("data", j)


def snapshot(host):
    cfg = get(host, "/config")
    dwell = get(host, "/settings/dwell_time")
    active = get(host, "/playsets/active")
    return {
        "config": redact(cfg),
        "dwell_time": dwell,
        "active_playset": active.get("active_playset"),
    }


def apply(host, show_fps=None, max_speed=None, brightness=None, rotation=None, dwell=None, playset=None):
    body = {}
    if show_fps is not None: body["show_fps"] = bool(show_fps)
    if max_speed is not None: body["max_speed_playback"] = bool(max_speed)
    if brightness is not None: body["brightness"] = int(brightness)
    if rotation is not None: body["rotation"] = int(rotation)
    if body:
        # ALWAYS merge=true: without it, PUT /config REPLACES the whole config
        # (API keys, sdcard_root, device_name gone). Learned the hard way on
        # 2026-08-29 (docs/jitter/LOG.md, "config wipe incident").
        r = requests.put(f"{host}/config?merge=true", json=body, timeout=15)
        r.raise_for_status()
        print("PUT /config?merge=true", body, "->", r.status_code)
    if dwell is not None:
        r = requests.put(f"{host}/settings/dwell_time", json={"dwell_time": int(dwell)}, timeout=15)
        r.raise_for_status()
        print("PUT /settings/dwell_time", dwell, "->", r.status_code)
    if playset:
        r = requests.post(f"{host}/playset/{urllib.parse.quote(playset)}", timeout=30)
        r.raise_for_status()
        print("POST /playset/", playset, "->", r.status_code)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["save", "restore", "set", "show"])
    ap.add_argument("run")
    ap.add_argument("--host", default="http://p3a-fab.local")
    ap.add_argument("--runs-dir", default=str(HERE / "runs"))
    ap.add_argument("--show-fps", type=int)
    ap.add_argument("--max-speed", type=int)
    ap.add_argument("--brightness", type=int)
    ap.add_argument("--rotation", type=int)
    ap.add_argument("--dwell", type=int)
    ap.add_argument("--playset")
    a = ap.parse_args()
    assert_dev_unit(a.host)

    run_dir = Path(a.runs_dir) / a.run
    run_dir.mkdir(parents=True, exist_ok=True)
    before = run_dir / "settings_before.json"

    if a.cmd == "save":
        s = snapshot(a.host)
        before.write_text(json.dumps(s, indent=1), encoding="utf-8")
        print(json.dumps(s, indent=1))
        return 0
    if a.cmd == "show":
        print(json.dumps(snapshot(a.host), indent=1))
        return 0
    changed_path = run_dir / "settings_changed.json"
    if a.cmd == "set":
        apply(a.host, a.show_fps, a.max_speed, a.brightness, a.rotation, a.dwell, a.playset)
        ch = json.loads(changed_path.read_text(encoding="utf-8")) if changed_path.exists() else {}
        for k, v in (("show_fps", a.show_fps), ("max_speed_playback", a.max_speed), ("brightness", a.brightness),
                     ("rotation", a.rotation), ("dwell_time", a.dwell), ("playset", a.playset)):
            if v is not None:
                ch[k] = True
        changed_path.write_text(json.dumps(ch), encoding="utf-8")
        return 0
    if a.cmd == "restore":
        if not before.exists():
            print(f"no {before}; nothing to restore")
            return 1
        s = json.loads(before.read_text(encoding="utf-8"))
        cfg = s.get("config", {})
        dwell = s.get("dwell_time", {})
        dwell_val = dwell.get("dwell_time") if isinstance(dwell, dict) else dwell
        ps = s.get("active_playset") or {}
        ps_name = ps.get("name") if isinstance(ps, dict) else None
        ch = json.loads(changed_path.read_text(encoding="utf-8")) if changed_path.exists() else {}
        if not ch:
            print("nothing was changed through this tool for this run; not touching the device")
            return 0
        # Restore ONLY the fields this tool changed (settings_changed.json), each via merge=true.
        apply(a.host,
              show_fps=cfg.get("show_fps") if ch.get("show_fps") else None,
              max_speed=cfg.get("max_speed_playback") if ch.get("max_speed_playback") else None,
              brightness=cfg.get("brightness") if ch.get("brightness") else None,
              rotation=cfg.get("rotation") if ch.get("rotation") else None,
              dwell=dwell_val if ch.get("dwell_time") else None,
              playset=ps_name if ch.get("playset") else None)
        changed_path.unlink()
        print("restored:", sorted(ch))
        return 0


if __name__ == "__main__":
    sys.exit(main())
