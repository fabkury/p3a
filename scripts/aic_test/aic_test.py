"""Off-device validator for the AIC channel pipeline described in
docs/superpowers/specs/2026-05-05-aic-apod-channels-design.md.

Runs four passes (public_domain x2, search "impressionism" x2). Each pass writes
a timestamped folder containing 25 image+metadata pairs and a run.json. After
both runs of a flavor finish, a compare.txt is written summarizing identical-vs-
different results between the two runs.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import random
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
USER_AGENT = "p3a-test/0.0.0 (+https://github.com/fkury/p3a)"
SEARCH_ENDPOINT = "https://api.artic.edu/api/v1/artworks/search"
IIIF_TEMPLATE = "https://www.artic.edu/iiif/2/{image_id}/full/1024,/0/default.jpg"
FIELDS = "id,title,image_id,date_display,source_updated_at,artist_display,medium_display,place_of_origin"
TARGET_COUNT = 25
PAGE_SIZE = 100


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


def http_post_json(url: str, params: dict[str, Any], body: dict[str, Any]) -> dict[str, Any]:
    """AIC's search endpoint accepts an Elasticsearch query DSL via POST body.
    The GET-only sort=field:direction syntax (in the spec) does not actually work
    against AIC's ES index — sort direction MUST come from the body."""
    headers = {"User-Agent": USER_AGENT, "Accept": "application/json", "Content-Type": "application/json"}
    if HAVE_REQUESTS:
        r = requests.post(url, params=params, json=body, headers=headers, timeout=30)
        r.raise_for_status()
        return r.json()
    qs = urllib.parse.urlencode(params, doseq=True)
    full = f"{url}?{qs}"
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(full, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def http_get_bytes(url: str) -> bytes:
    headers = {"User-Agent": USER_AGENT}
    if HAVE_REQUESTS:
        r = requests.get(url, headers=headers, timeout=60, stream=False)
        r.raise_for_status()
        return r.content
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def build_es_body(flavor: str, search_term: str | None) -> dict[str, Any]:
    """Build Elasticsearch query DSL body for AIC search. Common to both flavors."""
    if flavor == "public_domain":
        query: dict[str, Any] = {"term": {"is_public_domain": True}}
    elif flavor == "search":
        assert search_term, "search flavor requires search_term"
        query = {
            "bool": {
                "must": [
                    {"term": {"is_public_domain": True}},
                    {"query_string": {"query": search_term}},
                ]
            }
        }
    else:
        raise ValueError(f"unknown flavor: {flavor}")
    return {
        "query": query,
        "sort": [{"source_updated_at": {"order": "desc"}}],
        "fields": FIELDS.split(","),
    }


def build_url_params(page: int) -> dict[str, Any]:
    return {"limit": PAGE_SIZE, "page": page}


def pick_page_for_public_domain() -> int:
    """Spec says page=<random 1..N>. AIC actually caps offset (page*limit) at ~1000 —
    requesting deeper returns 403 'Invalid number of results'. With limit=100 that
    means usable pages are roughly [1, 10]. We probe page=1 to learn total_pages
    (informational, recorded in run.json) and then pick from the safe window."""
    probe = http_post_json(SEARCH_ENDPOINT, build_url_params(page=1), build_es_body("public_domain", None))
    total_pages = probe.get("pagination", {}).get("total_pages", 1)
    safe_max = max(1, min(10, total_pages))  # AIC offset cap ~1000 with limit=100
    chosen = random.randint(1, safe_max)
    return chosen


def slugify(s: str) -> str:
    out = []
    for ch in s.lower():
        if ch.isalnum():
            out.append(ch)
        elif ch in (" ", "-", "_"):
            out.append("_")
    return "".join(out).strip("_") or "x"


def run_pass(flavor: str, search_term: str | None, run_root: Path) -> dict[str, Any]:
    print(f"\n=== AIC pass: flavor={flavor} q={search_term!r} dir={run_root.name} ===")
    run_root.mkdir(parents=True, exist_ok=True)

    if flavor == "public_domain":
        page = pick_page_for_public_domain()
    else:
        page = 1

    url_params = build_url_params(page)
    body = build_es_body(flavor, search_term)
    print(f"  POST {SEARCH_ENDPOINT}")
    print(f"      page={page} (flavor={flavor})")
    response = http_post_json(SEARCH_ENDPOINT, url_params, body)
    raw_data = response.get("data", [])
    iiif_url = response.get("config", {}).get("iiif_url")
    pagination = response.get("pagination", {})
    print(f"  got {len(raw_data)} entries, total_pages={pagination.get('total_pages')}, iiif_url={iiif_url}")

    candidates = [e for e in raw_data if e.get("image_id")]
    print(f"  {len(candidates)} entries have a non-null image_id")

    entries: list[dict[str, Any]] = []
    successful_ids: list[int] = []
    successes = 0
    for idx, rec in enumerate(candidates):
        if successes >= TARGET_COUNT:
            break
        object_id = rec.get("id")
        image_id = rec.get("image_id")
        title = rec.get("title") or "(untitled)"
        url = (iiif_url or "https://www.artic.edu/iiif/2") + f"/{image_id}/full/1024,/0/default.jpg"

        slot = successes
        img_path = run_root / f"{slot:02d}_{object_id}.jpg"
        meta_path = run_root / f"{slot:02d}_{object_id}.json"

        status: dict[str, Any] = {
            "slot": slot,
            "object_id": object_id,
            "image_id": image_id,
            "title": title,
            "iiif_url": url,
        }
        try:
            t0 = time.time()
            data = http_get_bytes(url)
            elapsed = time.time() - t0
            img_path.write_bytes(data)
            sha = hashlib.sha256(data).hexdigest()
            status.update(
                ok=True,
                bytes=len(data),
                sha256=sha,
                elapsed_sec=round(elapsed, 3),
            )
            print(f"    [{slot:02d}] ok  id={object_id} bytes={len(data)} t={elapsed:.2f}s  {title[:60]}")
            successes += 1
            if object_id is not None:
                successful_ids.append(object_id)
        except Exception as exc:
            status.update(ok=False, error=f"{type(exc).__name__}: {exc}")
            print(f"    [{slot:02d}] ERR id={object_id} {exc}")

        entry_struct = {
            "object_id": object_id,
            "image_id": image_id,
            "extension": "jpg",
            "kind": 0,
            "source_updated_at_raw": rec.get("source_updated_at"),
        }
        meta_path.write_text(json.dumps(
            {"status": status, "raw": rec, "as_c_entry": entry_struct},
            indent=2, ensure_ascii=False,
        ), encoding="utf-8")

    summary: dict[str, Any] = {
        "timestamp_utc": dt.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "flavor": flavor,
        "search_term": search_term,
        "endpoint": SEARCH_ENDPOINT,
        "method": "POST",
        "page": page,
        "url_params": url_params,
        "body": body,
        "total_pages_from_api": pagination.get("total_pages"),
        "raw_data_count": len(raw_data),
        "candidates_with_image_id": len(candidates),
        "downloaded_count": successes,
        "object_ids": successful_ids,
        "user_agent": USER_AGENT,
        "iiif_url_from_api": iiif_url,
    }
    (run_root / "run.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"  saved {successes}/{TARGET_COUNT} images to {run_root}")
    return summary


def compare_runs(run_a_dir: Path, run_b_dir: Path, label: str, output_path: Path) -> None:
    summary_a = json.loads((run_a_dir / "run.json").read_text(encoding="utf-8"))
    summary_b = json.loads((run_b_dir / "run.json").read_text(encoding="utf-8"))
    ids_a = set(summary_a.get("object_ids") or [])
    ids_b = set(summary_b.get("object_ids") or [])
    common = ids_a & ids_b
    only_a = ids_a - ids_b
    only_b = ids_b - ids_a

    byte_equal = 0
    byte_diff = 0
    for oid in common:
        # Find the matching files (we know the index prefix differs but the suffix is _<id>.jpg)
        a_match = next(iter(run_a_dir.glob(f"*_{oid}.jpg")), None)
        b_match = next(iter(run_b_dir.glob(f"*_{oid}.jpg")), None)
        if not a_match or not b_match:
            continue
        sha_a = hashlib.sha256(a_match.read_bytes()).hexdigest()
        sha_b = hashlib.sha256(b_match.read_bytes()).hexdigest()
        if sha_a == sha_b:
            byte_equal += 1
        else:
            byte_diff += 1

    lines = [
        f"AIC compare ({label})",
        f"  run A: {run_a_dir.name}  ({len(ids_a)} ids)",
        f"  run B: {run_b_dir.name}  ({len(ids_b)} ids)",
        f"  common ids: {len(common)}",
        f"  only in A:  {len(only_a)}",
        f"  only in B:  {len(only_b)}",
        f"  byte-identical jpegs (over common ids): {byte_equal}",
        f"  byte-different  jpegs (over common ids): {byte_diff}",
        f"  verdict: {'IDENTICAL' if (ids_a == ids_b and byte_diff == 0) else 'DIFFERENT'}",
    ]
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


def main() -> int:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    runs: list[tuple[str, str | None, Path]] = []

    def stamp(label: str) -> Path:
        ts = dt.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        d = OUTPUT_ROOT / f"{label}__{ts}"
        # Ensure unique even if invoked twice within a second.
        i = 0
        while d.exists():
            i += 1
            d = OUTPUT_ROOT / f"{label}__{ts}_{i}"
        return d

    flavors: list[tuple[str, str | None, str]] = [
        ("public_domain", None, "public_domain"),
        ("public_domain", None, "public_domain"),
        ("search", "impressionism", "search_impressionism"),
        ("search", "impressionism", "search_impressionism"),
    ]

    for flavor, term, label in flavors:
        d = stamp(label)
        run_pass(flavor, term, d)
        runs.append((label, term, d))
        # small delay so timestamps differ and we are nice to the API
        time.sleep(2)

    # Group by label, compare consecutive pairs.
    by_label: dict[str, list[Path]] = {}
    for label, _term, d in runs:
        by_label.setdefault(label, []).append(d)

    for label, dirs in by_label.items():
        if len(dirs) >= 2:
            compare_path = OUTPUT_ROOT / f"compare_{label}.txt"
            compare_runs(dirs[0], dirs[1], label, compare_path)

    print(f"\nAll output in: {OUTPUT_ROOT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
