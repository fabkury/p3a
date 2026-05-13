"""LoC API — figure out the correct facet filter syntax.

The earlier probes showed `fa=` parameters being silently dropped from
search.url. We confirm the canonical query syntax by reading facet_trail
on each response: if the trail contains our requested facet/value, the
filter WAS applied; otherwise it was dropped.

We try several syntaxes per candidate facet value, then assert the trail.
"""

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
REPORT_PATH = OUTPUT_DIR / "facets.md"

API_ROOT = "https://www.loc.gov"

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev)",
    "Accept": "application/json",
})


def fetch(url: str, tag: str) -> dict[str, Any]:
    print(f"[{tag}] GET {url}")
    r = SESSION.get(url, timeout=60)
    out: dict[str, Any] = {"url": url, "status": r.status_code, "bytes": len(r.content)}
    try:
        j = r.json()
        (RAW_DIR / f"{tag}.json").write_text(
            json.dumps(j, indent=2)[:1_500_000], encoding="utf-8"
        )
        out["pagination_total"] = (j.get("pagination") or {}).get("total")
        out["facet_trail"] = j.get("facet_trail")
        out["search_url"] = (j.get("search") or {}).get("url")
        # First-result fingerprint to compare across queries.
        results = j.get("results") or []
        if results:
            r0 = results[0]
            out["first_id"] = r0.get("id")
            out["first_format"] = r0.get("original_format")
            out["first_partof"] = r0.get("partof")[:2] if isinstance(r0.get("partof"), list) else None
    except Exception as e:
        out["error"] = str(e)
    time.sleep(0.5)
    return out


def main() -> int:
    findings: dict[str, Any] = {}

    # ----- Facet param-key variants ------------------------------------------
    # The display title for the format facet is "Photo, Print, Drawing".
    # The LoC website docs use `fa=original-format:photo,+print,+drawing`
    # for these multi-word values. Try multiple permutations.
    candidates = [
        # (label, fa parameter value)
        ("fa_orig-fmt_lowercase_word", "original-format:photo, print, drawing"),
        ("fa_orig-fmt_lowercase_dash", "original-format:photo,+print,+drawing"),
        ("fa_orig-fmt_underscore_word", "original_format:photo, print, drawing"),
        ("fa_orig-fmt_title", "original-format:Photo, Print, Drawing"),
        ("fa_orig-fmt_simple_dash", "original-format:photo"),
        ("fa_orig-fmt_simple_underscore", "original_format:photo"),
        # Try with the field name `format`
        ("fa_format", "format:photo"),
        # Try `original-format:image` (Online Format facet maybe)
        ("fa_online-fmt_image", "online-format:image"),
        ("fa_online_format_image", "online_format:image"),
        # Try partof with various forms
        ("fa_partof_pp", "partof:prints and photographs division"),
        ("fa_partof_pp_dash", "partof:prints-and-photographs-division"),
        ("fa_partof_civil_war_maps", "partof:civil-war-maps"),
    ]
    findings["facet_param_tests"] = {}
    for label, fa in candidates:
        url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 25, 'sp': 1, 'fa': fa})}"
        findings["facet_param_tests"][label] = fetch(url, label)

    # ----- Path-based filtering: /collections/{slug}/ ------------------------
    findings["collection_path_tests"] = {}
    for slug in ["civil-war-maps", "panoramic-photographs"]:
        url = f"{API_ROOT}/collections/{slug}/?{urlencode({'fo': 'json', 'c': 25, 'sp': 1})}"
        findings["collection_path_tests"][slug] = fetch(url, f"coll_{slug}")

    # ----- Path-based filtering: /photos/ etc. -------------------------------
    findings["format_path_tests"] = {}
    for path in ["photos", "maps", "prints-photographs", "newspapers", "free-to-use"]:
        url = f"{API_ROOT}/{path}/?{urlencode({'fo': 'json', 'c': 25, 'sp': 1})}"
        findings["format_path_tests"][path] = fetch(url, f"path_{path}")

    # ----- Multi-fa: combine facets -----------------------------------------
    # Try ?fa=A&fa=B (multiple fa params).
    multi = [
        ("multi_pp_image", [
            "online-format:image",
            "partof:prints and photographs division",
        ]),
    ]
    findings["multi_facet_tests"] = {}
    for label, fas in multi:
        params = [("fo", "json"), ("c", "25"), ("sp", "1")]
        for f in fas:
            params.append(("fa", f))
        url = f"{API_ROOT}/search/?{urlencode(params)}"
        findings["multi_facet_tests"][label] = fetch(url, label)

    REPORT_PATH.write_text(json.dumps(findings, indent=2), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
