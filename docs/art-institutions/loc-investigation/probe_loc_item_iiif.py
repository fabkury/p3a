"""Fetch per-item JSON to discover the actual IIIF URL pattern."""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import requests

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
RAW_DIR = OUTPUT_DIR / "raw"
RAW_DIR.mkdir(parents=True, exist_ok=True)

API_ROOT = "https://www.loc.gov"

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev)",
    "Accept": "application/json",
})


def fetch_json(url: str, tag: str) -> Any:
    print(f"[{tag}] GET {url}")
    try:
        r = SESSION.get(url, timeout=60)
        try:
            j = r.json()
            (RAW_DIR / f"{tag}.json").write_text(
                json.dumps(j, indent=2)[:1_500_000], encoding="utf-8"
            )
            return j
        except Exception:
            (RAW_DIR / f"{tag}.json").write_text(r.text[:200_000], encoding="utf-8", errors="replace")
            return None
    except Exception as e:
        print(f"[{tag}]   exception: {e}")
        return None


def main() -> int:
    # Three items from the PPOC harvest.
    items = [
        ("2010635544", "I_2010635544"),  # Herblock cartoon  hlb.00055
        ("99404361", "I_99404361"),       # cph print
        ("2014648421", "I_2014648421"),   # photo
    ]
    findings = {}
    for item_id, tag in items:
        url = f"{API_ROOT}/item/{item_id}/?fo=json"
        j = fetch_json(url, tag)
        if not j:
            findings[item_id] = {"error": True}
            continue
        # Inspect item.resources and resources[].files[][] for IIIF URLs
        item = j.get("item") or {}
        resources = j.get("resources") or []
        out = {"top_keys": sorted(j.keys())[:30]}
        out["item_keys"] = sorted(item.keys())[:40]
        out["resources_count"] = len(resources)
        if resources:
            r0 = resources[0]
            out["resource_keys"] = sorted(r0.keys())
            out["resource_url"] = r0.get("url")
            out["resource_image"] = r0.get("image")
            files = r0.get("files") or []
            if isinstance(files, list) and files:
                f0 = files[0]
                if isinstance(f0, list) and f0:
                    out["files0_count"] = len(f0)
                    # Each file segment may have multiple representations
                    out["files00_keys"] = sorted(f0[0].keys()) if isinstance(f0[0], dict) else None
                    out["files00_sample"] = f0[0] if isinstance(f0[0], dict) else None
                    # Walk all files looking for IIIF URLs
                    iiif_found = []
                    for ff in f0:
                        if isinstance(ff, dict):
                            for k, v in ff.items():
                                if isinstance(v, str) and "image-services/iiif" in v:
                                    iiif_found.append({"key": k, "url": v[:200]})
                    out["iiif_urls_in_files0"] = iiif_found
        findings[item_id] = out
    OUTPUT_DIR.joinpath("item_iiif.md").write_text(json.dumps(findings, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
