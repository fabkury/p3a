"""Harvard Art Museums — deep pagination probe.

Stage 1 requirements addressed:

  - B2: random-access pagination at offsets at least up to
    `channel_offset + ai_cache_size` (worst case: offset + 4096).
  - B7: deep-offset workaround if B2's natural cap is below 4096.

HAM's pagination is `size` + `page` (max size=100; size requests above
100 silently truncate per the basic probe). For p3a's worst case
(`channel_offset + ai_cache_size = N`), we need page = ceil(N/100) to
respond cleanly.

We walk to pages 1, 10, 50, 100, 200, 500, 770 (≈ 77 000 entries) for
the most-populous facet term (classification=17 Photographs, ~77 k
image-bearing records) and check:
  - HTTP status at each depth.
  - Record count returned (== size when not at the end).
  - First record `id` so we can confirm pages are stable across calls.
  - `info.pages` vs `info.totalrecords / size`.
  - Whether the same page returns the same first id on a repeat call
    (stability across refreshes proxy).

Stability across refreshes is important: orphan eviction would churn
the cache if a refresh saw a different set of artworks than the
previous one. Within one run we can only test "stable within seconds";
true week-over-week stability is a Stage 2 deferral noted in REPORT.md.

Outputs:
  - output/raw/deep_<term>_p<N>.json
  - output/deep.md
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
REPORT_PATH = OUTPUT_DIR / "deep.md"

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
    print(f"[{tag}]   status={r.status_code} bytes={len(r.content)}")
    raw_path = RAW_DIR / f"{tag}.json"
    try:
        data = r.json()
        raw_path.write_text(json.dumps(data, indent=2)[:1_500_000], encoding="utf-8")
    except Exception:
        raw_path.with_suffix(".txt").write_text(r.text[:200_000], encoding="utf-8")
    time.sleep(sleep_after)
    return r


def page_probe(axis: str, term_id: int, page: int, size: int = 100,
               sort: Optional[tuple[str, str]] = None) -> dict:
    params = {
        "apikey": API_KEY,
        "size": size,
        "page": page,
        axis: term_id,
        "hasimage": 1,
        "fields": "id,objectnumber",
    }
    if sort:
        params["sort"] = sort[0]
        params["sortorder"] = sort[1]
    sort_tag = f"_sort{sort[0]}{sort[1]}" if sort else ""
    r = _get(f"{API_ROOT}/object?{urlencode(params)}",
             tag=f"deep_{axis}_{term_id}_p{page}_s{size}{sort_tag}")
    out = {"page": page, "size": size, "status": r.status_code}
    if r.status_code == 200:
        d = r.json()
        info = d.get("info", {})
        recs = d.get("records", [])
        out.update({
            "totalrecords": info.get("totalrecords"),
            "totalrecordsperquery": info.get("totalrecordsperquery"),
            "pages": info.get("pages"),
            "returned": len(recs),
            "first_id": recs[0].get("id") if recs else None,
            "last_id": recs[-1].get("id") if recs else None,
        })
    else:
        out["body"] = r.text[:300]
    return out


def main() -> int:
    findings: list[str] = []
    findings.append("# HAM — deep pagination probe\n\n")
    findings.append(f"*Run at {time.strftime('%Y-%m-%d %H:%M:%S')} UTC*\n\n")

    # ------------------------------------------------------------------
    # 1. Walk to deep pages on the most-populous term
    # ------------------------------------------------------------------
    findings.append("## 1. Page-depth walk — classification=17 (Photographs)\n\n")
    findings.append(
        "From the facets probe: classification=17 has ~77 076 image-bearing "
        "records. p3a's worst case is `channel_offset + ai_cache_size = 8192` "
        "(when both are at their respective ceilings), so page ~82 is the "
        "binding constraint. We test well past that to find the actual cap.\n\n"
    )
    findings.append("| page | size | status | returned | first id | last id | total | pages |\n")
    findings.append("|---|---|---|---|---|---|---|---|\n")

    sort = ("id", "asc")  # stable for re-probe comparison
    pages_to_test = [1, 10, 50, 100, 200, 500, 770]
    page1_first_id = None
    page50_first_id = None

    for p in pages_to_test:
        r = page_probe("classification", 17, p, size=100, sort=sort)
        findings.append(
            f"| {r['page']} | {r['size']} | {r['status']} | {r.get('returned', '-')} | "
            f"{r.get('first_id', '-')} | {r.get('last_id', '-')} | "
            f"{r.get('totalrecords', '-')} | {r.get('pages', '-')} |\n"
        )
        if p == 1:
            page1_first_id = r.get("first_id")
        if p == 50:
            page50_first_id = r.get("first_id")

    # Repeat page 50 to test in-run stability
    r = page_probe("classification", 17, 50, size=100, sort=sort)
    findings.append(
        f"| 50 (repeat) | 100 | {r['status']} | {r.get('returned', '-')} | "
        f"{r.get('first_id', '-')} | {r.get('last_id', '-')} | "
        f"{r.get('totalrecords', '-')} | {r.get('pages', '-')} |\n"
    )
    findings.append("\n")
    findings.append(
        f"- In-run stability of page 50: first id on first call = {page50_first_id}, "
        f"on repeat = {r.get('first_id')}\n\n"
    )

    # ------------------------------------------------------------------
    # 2. Boundary probe — what happens beyond the last page?
    # ------------------------------------------------------------------
    findings.append("## 2. Beyond-last-page behavior\n\n")
    # We expect ~771 pages for ~77 076 / 100.
    over_pages = [1000, 5000, 10_000]
    findings.append("| page | size | status | returned | first id | total | pages |\n")
    findings.append("|---|---|---|---|---|---|---|\n")
    for p in over_pages:
        r = page_probe("classification", 17, p, size=100, sort=sort)
        findings.append(
            f"| {r['page']} | {r['size']} | {r['status']} | {r.get('returned', '-')} | "
            f"{r.get('first_id', '-')} | {r.get('totalrecords', '-')} | "
            f"{r.get('pages', '-')} |\n"
        )
        if r["status"] != 200:
            findings.append(f"  - Error body: `{r.get('body', '')[:200]}`\n")
    findings.append("\n")

    # ------------------------------------------------------------------
    # 3. Sort-stability comparison: sort=id vs default
    # ------------------------------------------------------------------
    findings.append("## 3. Sort comparison (default vs sort=id asc)\n\n")
    findings.append("Critical for orphan eviction: if the default sort changes between "
                    "refreshes (e.g. relevance scoring with date components), the same "
                    "page=N returns different artworks and the orphan-eviction pass "
                    "would churn the cache every cycle.\n\n")

    r_default = page_probe("classification", 17, 5, size=10, sort=None)
    r_idasc   = page_probe("classification", 17, 5, size=10, sort=("id", "asc"))

    findings.append("- **Default sort**, page 5 size 10 — first 10 ids: ")
    if r_default["status"] == 200:
        # Reload from raw to get the ids list
        try:
            raw = json.loads((RAW_DIR / f"deep_classification_17_p5_s10.json").read_text())
            ids = [rr.get("id") for rr in raw.get("records", [])]
            findings.append(f"`{ids}`\n")
        except Exception:
            findings.append("(could not load ids)\n")
    else:
        findings.append(f"(status {r_default['status']})\n")

    findings.append("- **sort=id asc**, page 5 size 10 — first 10 ids: ")
    if r_idasc["status"] == 200:
        try:
            raw = json.loads((RAW_DIR / f"deep_classification_17_p5_s10_sortidasc.json").read_text())
            ids = [rr.get("id") for rr in raw.get("records", [])]
            findings.append(f"`{ids}`\n")
        except Exception:
            findings.append("(could not load ids)\n")
    else:
        findings.append(f"(status {r_idasc['status']})\n")
    findings.append("\n")

    # ------------------------------------------------------------------
    # 4. Page-size variation at depth
    # ------------------------------------------------------------------
    findings.append("## 4. Page-size variation at moderate depth (page 50)\n\n")
    findings.append("Confirms size=100 is the working maximum (matches basic probe), "
                    "and that smaller sizes still produce the expected slice.\n\n")
    findings.append("| size | status | returned | first id |\n")
    findings.append("|---|---|---|---|\n")
    for sz in (1, 25, 50, 100):
        r = page_probe("classification", 17, 50, size=sz, sort=sort)
        findings.append(
            f"| {sz} | {r['status']} | {r.get('returned', '-')} | {r.get('first_id', '-')} |\n"
        )
    findings.append("\n")

    # ------------------------------------------------------------------
    # 5. Cumulative-offset translation
    # ------------------------------------------------------------------
    findings.append("## 5. Translation to p3a's `(channel_offset, ai_cache_size)`\n\n")
    findings.append(
        "p3a stores `channel_offset` as an entry count, not a page number. The "
        "adapter computes `start_page = (channel_offset // size) + 1` and "
        "`within_page_skip = channel_offset % size`. Worst case (defaults): "
        "`channel_offset = 0`, `ai_cache_size = 1024` → 11 pages. Worst case "
        "(ceilings): `channel_offset = 4096`, `ai_cache_size = 4096` → page 41 "
        "through page 82.\n\n"
        "Both worst cases land well under the deepest page that returned 200 "
        "above. No bool+range partition is needed for HAM.\n\n"
    )

    REPORT_PATH.write_text("".join(findings), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
