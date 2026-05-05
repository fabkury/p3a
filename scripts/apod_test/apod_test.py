"""Off-device validator for the APOD channel pipeline described in
docs/superpowers/specs/2026-05-05-aic-apod-channels-design.md.

Runs two passes. Each pass walks back 25 calendar days from today (UTC), fetches
metadata, downloads images for image days, writes a skip-marker JSON for video
days, and finishes with a run.json. After both passes, writes compare.txt.

API key is loaded from (in order): --api-key CLI flag, NASA_APOD_API_KEY env var,
or scripts/apod_test/.env file (KEY=VALUE format). The .env file is gitignored.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

try:
    import requests  # type: ignore[import-not-found]
    HAVE_REQUESTS = True
except ImportError:
    HAVE_REQUESTS = False
    import urllib.error
    import urllib.parse
    import urllib.request


SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_ROOT = SCRIPT_DIR / "output"
DOTENV_PATH = SCRIPT_DIR / ".env"
USER_AGENT = "p3a-test/0.0.0 (+https://github.com/fkury/p3a)"
APOD_ENDPOINT = "https://api.nasa.gov/planetary/apod"
HISTORY_DAYS = 25
MAX_IMAGE_BYTES = 16 * 1024 * 1024  # spec §4


def load_api_key(cli_key: str | None) -> str:
    if cli_key:
        return cli_key
    env_key = os.environ.get("NASA_APOD_API_KEY")
    if env_key:
        return env_key
    if DOTENV_PATH.exists():
        for line in DOTENV_PATH.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                if k.strip() == "NASA_APOD_API_KEY":
                    return v.strip().strip('"').strip("'")
    raise SystemExit(
        "No APOD API key found. Set NASA_APOD_API_KEY, write scripts/apod_test/.env, or pass --api-key."
    )


def http_get_json(url: str, params: dict[str, Any]) -> dict[str, Any]:
    headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
    if HAVE_REQUESTS:
        r = requests.get(url, params=params, headers=headers, timeout=30)
        r.raise_for_status()
        return r.json()
    qs = urllib.parse.urlencode(params, doseq=True)
    full = f"{url}?{qs}"
    req = urllib.request.Request(full, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def http_get_bytes(url: str, max_bytes: int) -> bytes:
    headers = {"User-Agent": USER_AGENT}
    if HAVE_REQUESTS:
        r = requests.get(url, headers=headers, timeout=120, stream=True)
        r.raise_for_status()
        buf = bytearray()
        for chunk in r.iter_content(chunk_size=64 * 1024):
            buf.extend(chunk)
            if len(buf) > max_bytes:
                raise RuntimeError(f"image exceeded cap of {max_bytes} bytes")
        return bytes(buf)
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=120) as resp:
        buf = bytearray()
        while True:
            chunk = resp.read(64 * 1024)
            if not chunk:
                break
            buf.extend(chunk)
            if len(buf) > max_bytes:
                raise RuntimeError(f"image exceeded cap of {max_bytes} bytes")
        return bytes(buf)


def sniff_image_format(buf: bytes) -> str | None:
    """Return 'jpg' / 'png' / 'gif' / 'webp' or None per spec §4 magic-byte sniff (first 16 bytes)."""
    head = buf[:16]
    if len(head) < 12:
        return None
    if head[:3] == b"\xff\xd8\xff":
        return "jpg"
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if head[:6] in (b"GIF87a", b"GIF89a"):
        return "gif"
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        return "webp"
    return None


def url_has_supported_image_extension(url: str) -> bool:
    lower = url.lower().split("?", 1)[0]
    return lower.endswith((".jpg", ".jpeg", ".png", ".gif", ".webp"))


def fetch_one_day(day: dt.date, api_key: str, run_root: Path, slot: int) -> dict[str, Any]:
    date_str = day.isoformat()
    print(f"  [{slot:02d}] {date_str} fetching metadata...")

    status: dict[str, Any] = {"slot": slot, "date": date_str, "endpoint": APOD_ENDPOINT}
    try:
        meta = http_get_json(APOD_ENDPOINT, {"api_key": api_key, "date": date_str})
    except Exception as exc:
        status.update(ok=False, kind="metadata_error", error=f"{type(exc).__name__}: {exc}")
        print(f"        ERR  {exc}")
        meta_path = run_root / f"{slot:02d}_{date_str}.json"
        meta_path.write_text(json.dumps({"status": status, "raw": None}, indent=2, ensure_ascii=False), encoding="utf-8")
        return status

    media_type = meta.get("media_type")
    title = meta.get("title") or "(untitled)"

    # Spec §4: video days get a skip-marker entry with kind=1.
    if media_type != "image":
        status.update(
            ok=True,
            kind="skip_marker",
            media_type=media_type,
            title=title,
        )
        meta_path = run_root / f"{slot:02d}_{date_str}.json"
        entry_struct = {
            "date": date_str,
            "extension": None,
            "kind": 1,  # spec: video skip-marker
            "title": title[:48],
        }
        meta_path.write_text(json.dumps({"status": status, "raw": meta, "as_c_entry": entry_struct}, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"        skip media_type={media_type!r} title={title[:60]!r}")
        return status

    # Image day. Pick URL per spec §4: hdurl if present and ends in supported extension, else url.
    hdurl = meta.get("hdurl")
    url = meta.get("url")
    chosen_url = hdurl if (hdurl and url_has_supported_image_extension(hdurl)) else url
    if not chosen_url:
        status.update(ok=False, kind="dead", error="no usable url")
        meta_path = run_root / f"{slot:02d}_{date_str}.json"
        meta_path.write_text(json.dumps({"status": status, "raw": meta}, indent=2, ensure_ascii=False), encoding="utf-8")
        return status

    status["chosen_url"] = chosen_url
    status["hdurl"] = hdurl
    status["url"] = url
    status["title"] = title

    # Download with cap, then magic-byte sniff.
    try:
        t0 = time.time()
        data = http_get_bytes(chosen_url, MAX_IMAGE_BYTES)
        elapsed = time.time() - t0
    except Exception as exc:
        status.update(ok=False, kind="download_error", error=f"{type(exc).__name__}: {exc}")
        meta_path = run_root / f"{slot:02d}_{date_str}.json"
        meta_path.write_text(json.dumps({"status": status, "raw": meta}, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"        ERR download: {exc}")
        return status

    fmt = sniff_image_format(data)
    if fmt is None:
        status.update(
            ok=False,
            kind="dead",
            bytes=len(data),
            error="magic-byte sniff failed (not JPEG/PNG/GIF/WebP)",
        )
        meta_path = run_root / f"{slot:02d}_{date_str}.json"
        entry_struct = {
            "date": date_str,
            "extension": None,
            "kind": 0xFF,  # dead per spec
            "title": title[:48],
        }
        meta_path.write_text(json.dumps({"status": status, "raw": meta, "as_c_entry": entry_struct}, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"        ERR magic-byte sniff failed (got {data[:8]!r})")
        return status

    img_path = run_root / f"{slot:02d}_{date_str}.{fmt}"
    tmp_path = img_path.with_suffix(img_path.suffix + ".tmp")
    tmp_path.write_bytes(data)
    os.replace(tmp_path, img_path)  # atomic-ish

    sha = hashlib.sha256(data).hexdigest()
    status.update(
        ok=True,
        kind="image",
        format=fmt,
        bytes=len(data),
        sha256=sha,
        elapsed_sec=round(elapsed, 3),
        title=title,
    )
    print(f"        ok   fmt={fmt} bytes={len(data)} t={elapsed:.2f}s  {title[:60]}")

    entry_struct = {
        "date": date_str,
        "extension": fmt,
        "kind": 0,
        "title": title[:48],
    }
    meta_path = run_root / f"{slot:02d}_{date_str}.json"
    meta_path.write_text(json.dumps({"status": status, "raw": meta, "as_c_entry": entry_struct}, indent=2, ensure_ascii=False), encoding="utf-8")
    return status


def run_pass(api_key: str, run_root: Path) -> dict[str, Any]:
    print(f"\n=== APOD pass: dir={run_root.name} ===")
    run_root.mkdir(parents=True, exist_ok=True)

    today = dt.datetime.utcnow().date()
    days = [today - dt.timedelta(days=i) for i in range(HISTORY_DAYS)]

    statuses: list[dict[str, Any]] = []
    for slot, day in enumerate(days):
        status = fetch_one_day(day, api_key, run_root, slot)
        statuses.append(status)
        # Be nice to api.nasa.gov; personal key is 1000/hr but we have plenty of headroom.
        time.sleep(1.0)

    image_count = sum(1 for s in statuses if s.get("ok") and s.get("kind") == "image")
    skip_count = sum(1 for s in statuses if s.get("ok") and s.get("kind") == "skip_marker")
    dead_count = sum(1 for s in statuses if not s.get("ok"))

    summary: dict[str, Any] = {
        "timestamp_utc": dt.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "endpoint": APOD_ENDPOINT,
        "history_days": HISTORY_DAYS,
        "today_utc": today.isoformat(),
        "image_count": image_count,
        "skip_marker_count": skip_count,
        "dead_count": dead_count,
        "user_agent": USER_AGENT,
        "entries": [
            {
                "slot": s["slot"],
                "date": s["date"],
                "ok": s.get("ok"),
                "kind": s.get("kind"),
                "format": s.get("format"),
                "bytes": s.get("bytes"),
                "sha256": s.get("sha256"),
                "title": s.get("title"),
            }
            for s in statuses
        ],
    }
    (run_root / "run.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"  saved {image_count} images, {skip_count} skip-markers, {dead_count} errors to {run_root}")
    return summary


def compare_runs(run_a_dir: Path, run_b_dir: Path, output_path: Path) -> None:
    summary_a = json.loads((run_a_dir / "run.json").read_text(encoding="utf-8"))
    summary_b = json.loads((run_b_dir / "run.json").read_text(encoding="utf-8"))
    entries_a = {e["date"]: e for e in summary_a["entries"]}
    entries_b = {e["date"]: e for e in summary_b["entries"]}
    common_dates = sorted(set(entries_a) & set(entries_b))

    sha_match = 0
    sha_diff = 0
    kind_diff_dates: list[str] = []
    for d in common_dates:
        ea = entries_a[d]
        eb = entries_b[d]
        if ea.get("kind") != eb.get("kind"):
            kind_diff_dates.append(d)
            continue
        if ea.get("kind") == "image":
            if ea.get("sha256") and ea.get("sha256") == eb.get("sha256"):
                sha_match += 1
            else:
                sha_diff += 1

    lines = [
        "APOD compare",
        f"  run A: {run_a_dir.name}  images={summary_a['image_count']} skips={summary_a['skip_marker_count']}",
        f"  run B: {run_b_dir.name}  images={summary_b['image_count']} skips={summary_b['skip_marker_count']}",
        f"  common dates: {len(common_dates)}",
        f"  byte-identical images (sha256): {sha_match}",
        f"  byte-different  images (sha256): {sha_diff}",
        f"  dates whose kind differs (image vs skip vs error): {len(kind_diff_dates)}",
    ]
    if kind_diff_dates:
        lines.append(f"    -> {kind_diff_dates}")
    verdict = "IDENTICAL" if (sha_diff == 0 and not kind_diff_dates) else "DIFFERENT"
    lines.append(f"  verdict: {verdict}")
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--api-key", help="NASA APOD API key (overrides env / .env)")
    args = parser.parse_args()

    api_key = load_api_key(args.api_key)
    masked = api_key[:4] + "..." + api_key[-4:] if len(api_key) >= 8 else "***"
    print(f"Using APOD api_key={masked}")

    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    runs: list[Path] = []
    for _ in range(2):
        ts = dt.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        d = OUTPUT_ROOT / f"daily__{ts}"
        i = 0
        while d.exists():
            i += 1
            d = OUTPUT_ROOT / f"daily__{ts}_{i}"
        run_pass(api_key, d)
        runs.append(d)

    if len(runs) >= 2:
        compare_runs(runs[0], runs[1], OUTPUT_ROOT / "compare.txt")

    print(f"\nAll output in: {OUTPUT_ROOT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
