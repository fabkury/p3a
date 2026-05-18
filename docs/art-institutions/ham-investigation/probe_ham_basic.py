"""Harvard Art Museums — basic surface probe.

Exercises the items in `requirements.md` that don't need specialized
walkers:

  - D1/D2 API-key model: does a missing/invalid key return 401, 403, or
    silently degrade? Is the key required on every endpoint?
  - B1 HTTPS + TLS: does the chain validate with the default trust store
    (proxy for whether esp_crt_bundle will cover it)?
  - A3/A4 listing shape: GET /object with size, page, hasimage=1, fields,
    and a representative classification filter. Confirm JSON content-type,
    presence of `info.totalrecords`, and the per-record metadata fields
    the browse UI needs (title, people, dated, primaryimageurl).
  - A5/A7 caption metadata + thumbnail viability: confirm title /
    artist / date inline on the listing and that primaryimageurl is a
    fetchable IIIF URL.
  - B3 total record count: confirm `info.totalrecords` matches expected
    shape.
  - B4 rate-limit signaling: read documented limit (2500 req/day) and
    look for response headers that surface per-key remaining quota.

Outputs:
  - output/raw/*.json — captured response bodies for reproducibility.
  - output/basic.md — markdown summary with the answers per requirement.
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from typing import Any, Optional
from urllib.parse import urlencode

import requests

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
RAW_DIR = OUTPUT_DIR / "raw"
OUTPUT_DIR.mkdir(exist_ok=True)
RAW_DIR.mkdir(exist_ok=True)
REPORT_PATH = OUTPUT_DIR / "basic.md"

API_ROOT = "https://api.harvardartmuseums.org"
API_KEY = os.environ.get("HAM_API_KEY", "c09e5b21-5ea4-4762-b611-e41d3a2ba07d")

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev; testing HAM integration)",
    "Accept": "application/json",
})


def _get(url: str, *, tag: str, sleep_after: float = 0.4) -> requests.Response:
    print(f"[{tag}] GET {url}")
    r = SESSION.get(url, timeout=30)
    print(f"[{tag}]   status={r.status_code} bytes={len(r.content)} "
          f"content-type={r.headers.get('Content-Type', '')}")
    # Save raw response (truncated if very large).
    raw_path = RAW_DIR / f"{tag}.json"
    try:
        parsed = r.json()
        raw_path.write_text(json.dumps(parsed, indent=2)[:1_000_000], encoding="utf-8")
    except Exception:
        raw_path.with_suffix(".txt").write_text(r.text[:200_000], encoding="utf-8")
    time.sleep(sleep_after)
    return r


def _md_kv(k: str, v: Any) -> str:
    return f"- **{k}**: {v}\n"


def main() -> int:
    findings: list[str] = []
    findings.append("# HAM — basic surface probe\n\n")
    findings.append(f"*Run at {time.strftime('%Y-%m-%d %H:%M:%S')} UTC*\n\n")

    # ------------------------------------------------------------------
    # 1. Auth — does the API require the key? What happens without it?
    # ------------------------------------------------------------------
    findings.append("## 1. Authentication model (D1/D2)\n\n")

    # 1a) No key at all
    r = _get(f"{API_ROOT}/object?size=1", tag="auth_no_key")
    findings.append(_md_kv("GET /object without apikey", f"HTTP {r.status_code}"))
    findings.append(f"  - Response excerpt: `{r.text[:200].replace(chr(10), ' ')}…`\n")

    # 1b) Invalid key
    r = _get(f"{API_ROOT}/object?size=1&apikey=invalid-key-test", tag="auth_invalid_key")
    findings.append(_md_kv("GET /object with invalid apikey", f"HTTP {r.status_code}"))
    findings.append(f"  - Response excerpt: `{r.text[:200].replace(chr(10), ' ')}…`\n")

    # 1c) Valid key
    r = _get(f"{API_ROOT}/object?size=1&apikey={API_KEY}", tag="auth_valid_key")
    findings.append(_md_kv("GET /object with valid apikey", f"HTTP {r.status_code}"))
    if r.status_code == 200:
        findings.append("  - Headers (interesting subset):\n")
        for h in ("X-RateLimit-Limit", "X-RateLimit-Remaining", "X-RateLimit-Reset",
                  "Retry-After", "Content-Type", "Access-Control-Allow-Origin"):
            v = r.headers.get(h)
            if v is not None:
                findings.append(f"    - `{h}: {v}`\n")

    findings.append("\n")

    # ------------------------------------------------------------------
    # 2. Listing envelope shape and metadata fields (A3/A5/B3/B6)
    # ------------------------------------------------------------------
    findings.append("## 2. Listing envelope and metadata (A3, A5, B3, B6)\n\n")

    params = {
        "apikey": API_KEY,
        "size": 5,
        "page": 1,
        "hasimage": 1,
        "classification": 26,  # 26 = Paintings (well-known HAM id; we'll verify in facets probe)
        "fields": "id,objectnumber,title,dated,people,classification,primaryimageurl,images,url",
    }
    r = _get(f"{API_ROOT}/object?{urlencode(params)}", tag="list_paintings_p1")
    if r.status_code == 200:
        data = r.json()
        info = data.get("info", {})
        recs = data.get("records", [])
        findings.append("### Envelope `info` block\n\n")
        findings.append("```json\n" + json.dumps(info, indent=2) + "\n```\n\n")
        findings.append(f"- **records count returned**: {len(recs)}\n")
        if recs:
            sample = recs[0]
            findings.append("### Per-record fields (first record)\n\n")
            findings.append("```json\n" + json.dumps(sample, indent=2)[:2000] + "\n```\n\n")
            # Spot-check the fields p3a needs:
            findings.append("**Field availability sanity check on the 5 returned records:**\n\n")
            checks = ["title", "dated", "primaryimageurl", "people", "classification", "id"]
            for f in checks:
                present = sum(1 for rr in recs if rr.get(f))
                findings.append(f"  - `{f}`: present on {present}/{len(recs)} records\n")
            findings.append("\n")
    else:
        findings.append(f"- (listing probe failed with HTTP {r.status_code})\n\n")

    # ------------------------------------------------------------------
    # 3. hasimage=0 vs hasimage=1 (A4)
    # ------------------------------------------------------------------
    findings.append("## 3. hasimage filter (A4)\n\n")

    base_params = {"apikey": API_KEY, "size": 1, "classification": 26}
    for h in (0, 1):
        params = {**base_params, "hasimage": h}
        r = _get(f"{API_ROOT}/object?{urlencode(params)}", tag=f"hasimage_{h}")
        if r.status_code == 200:
            info = r.json().get("info", {})
            findings.append(f"- classification=26 hasimage={h}: totalrecords = "
                            f"`{info.get('totalrecords')}`\n")

    # Without the filter at all
    params = {"apikey": API_KEY, "size": 1, "classification": 26}
    r = _get(f"{API_ROOT}/object?{urlencode(params)}", tag="hasimage_omitted")
    if r.status_code == 200:
        info = r.json().get("info", {})
        findings.append(f"- classification=26 (no hasimage param): totalrecords = "
                        f"`{info.get('totalrecords')}`\n")
    findings.append("\n")

    # ------------------------------------------------------------------
    # 4. Sort options (preparation for offset stability — B5)
    # ------------------------------------------------------------------
    findings.append("## 4. Sort options (B5 prep)\n\n")

    sort_tests = [
        ("id_asc",       {"sort": "id",        "sortorder": "asc"}),
        ("dated_asc",    {"sort": "dated",     "sortorder": "asc"}),
        ("rank_default", {}),
    ]
    for label, extra in sort_tests:
        params = {**{"apikey": API_KEY, "size": 3, "classification": 26, "hasimage": 1,
                     "fields": "id,objectnumber,title,dated"}, **extra}
        r = _get(f"{API_ROOT}/object?{urlencode(params)}", tag=f"sort_{label}")
        if r.status_code == 200:
            recs = r.json().get("records", [])
            ids = [rr.get("id") for rr in recs]
            findings.append(f"- sort={label}: first 3 object ids = {ids}\n")
    findings.append("\n")

    # ------------------------------------------------------------------
    # 5. Page-size ceiling probe (B2 sanity)
    # ------------------------------------------------------------------
    findings.append("## 5. Page-size ceiling (B2 sanity)\n\n")
    for sz in (10, 100, 200, 500):
        params = {"apikey": API_KEY, "size": sz, "classification": 26, "hasimage": 1,
                  "fields": "id"}
        r = _get(f"{API_ROOT}/object?{urlencode(params)}", tag=f"size_{sz}")
        if r.status_code == 200:
            info = r.json().get("info", {})
            recs = r.json().get("records", [])
            tprq = info.get("totalrecordsperquery")
            findings.append(f"- size={sz}: HTTP 200, returned {len(recs)} records, "
                            f"`info.totalrecordsperquery`={tprq}\n")
        else:
            findings.append(f"- size={sz}: HTTP {r.status_code} (excerpt: "
                            f"`{r.text[:120].replace(chr(10), ' ')}`)\n")
    findings.append("\n")

    # ------------------------------------------------------------------
    # 6. IIIF / primary image accessibility (A7, C1 prep)
    # ------------------------------------------------------------------
    findings.append("## 6. Primary image URL — fetchable? (A7, C1 prep)\n\n")
    params = {"apikey": API_KEY, "size": 3, "classification": 26, "hasimage": 1,
              "fields": "id,primaryimageurl,images"}
    r = _get(f"{API_ROOT}/object?{urlencode(params)}", tag="img_sample")
    if r.status_code == 200:
        recs = r.json().get("records", [])
        for rr in recs:
            pu = rr.get("primaryimageurl")
            imgs = rr.get("images", [])
            baseimage = imgs[0].get("baseimageurl") if imgs else None
            findings.append(f"- object id={rr.get('id')}: primaryimageurl=`{pu}`, "
                            f"images[0].baseimageurl=`{baseimage}`\n")

    findings.append("\n")
    REPORT_PATH.write_text("".join(findings), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
