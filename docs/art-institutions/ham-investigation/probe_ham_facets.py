"""Harvard Art Museums — facet axis inventory probe.

Stage 1 requirements addressed:

  - A1: enumerate HAM's full facet/aggregation surface.
  - A2: for each candidate axis, list terms with image-bearing counts;
    verify counts are obtainable in ≤ 1 request per axis OR via a bounded
    per-term probe.

HAM exposes these top-level resource endpoints that double as facet
vocabularies: classification, century, color, culture, gallery, group,
medium, period, place, technique, worktype, person.

For each, this script:
  1. Fetches the term list (size=100, page=1) and notes `info.totalrecords`
     (the count of terms in that vocabulary).
  2. Reads each term's documented `objectcount` field — that's the
     all-records count, not the image-bearing count. We confirm it exists.
  3. Probes the image-bearing count for the top-3 most-populous terms by
     issuing `/object?<axis>=<id>&hasimage=1&size=1` and reading
     `info.totalrecords`. This mirrors AIC's per-term count probe.

The axes are then ranked by:
  - whether the documented `objectcount` is populated,
  - whether term labels fit in the 32-char identifier slot,
  - whether per-axis term enumeration fits in a single page (size=100),
  - whether the image-bearing fraction is reasonable (>10 %).

Outputs:
  - output/raw/facets_<axis>_terms.json
  - output/raw/facets_<axis>_term_<termid>_imgcount.json
  - output/facets.md — per-axis summary + recommendation
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
REPORT_PATH = OUTPUT_DIR / "facets.md"

API_ROOT = "https://api.harvardartmuseums.org"
API_KEY = os.environ.get("HAM_API_KEY", "c09e5b21-5ea4-4762-b611-e41d3a2ba07d")

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev; testing HAM integration)",
    "Accept": "application/json",
})

# Axes worth inventorying. The endpoint name is also the filter param name
# on /object (the HAM convention). Per the api-docs README: classification,
# century, color, culture, gallery, group, medium, period, place, technique,
# worktype, person.
AXES = [
    "classification",
    "century",
    "culture",
    "gallery",
    "period",
    "technique",
    "worktype",
    "place",
    "medium",
    "color",
    "group",
    "person",
]


def _get_json(url: str, *, tag: str, sleep_after: float = 0.3) -> Optional[dict]:
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
        return None
    time.sleep(sleep_after)
    if r.status_code != 200:
        return None
    return data


def fetch_terms(axis: str) -> tuple[int, list[dict]]:
    """Return (total_terms, list_of_term_dicts). Each term has at least id/name."""
    # Sort by objectcount desc so we get the most populous terms in page 1.
    params = {
        "apikey": API_KEY,
        "size": 100,
        "page": 1,
        "sort": "objectcount",
        "sortorder": "desc",
    }
    data = _get_json(f"{API_ROOT}/{axis}?{urlencode(params)}", tag=f"facets_{axis}_terms")
    if not data:
        return 0, []
    info = data.get("info", {})
    recs = data.get("records", [])
    return int(info.get("totalrecords", 0)), recs


def probe_term_image_count(axis: str, term_id: int) -> Optional[int]:
    """Issue /object?<axis>=<id>&hasimage=1&size=1 and return info.totalrecords."""
    params = {
        "apikey": API_KEY,
        "size": 1,
        axis: term_id,
        "hasimage": 1,
        "fields": "id",
    }
    data = _get_json(f"{API_ROOT}/object?{urlencode(params)}",
                     tag=f"facets_{axis}_term_{term_id}_imgcount")
    if not data:
        return None
    return int(data.get("info", {}).get("totalrecords", 0))


def main() -> int:
    findings: list[str] = []
    findings.append("# HAM — facet axis inventory\n\n")
    findings.append(f"*Run at {time.strftime('%Y-%m-%d %H:%M:%S')} UTC*\n\n")
    findings.append(
        "Enumerates each candidate facet axis and probes image-bearing counts "
        "for the top-3 most-populous terms in each. Used to choose the 1–4 "
        "axes that ship in HAM v1.\n\n"
    )

    summary_rows: list[dict] = []

    for axis in AXES:
        findings.append(f"## Axis: `{axis}`\n\n")
        total, recs = fetch_terms(axis)
        findings.append(f"- Total terms in vocabulary: **{total}**\n")
        findings.append(f"- Returned in page 1 (size=100): **{len(recs)}**\n")

        # Check what fields each term carries
        if recs:
            sample = recs[0]
            findings.append(f"- Sample term fields: `{sorted(sample.keys())}`\n")

            # Count terms with objectcount > 0
            with_count = [r for r in recs if isinstance(r.get("objectcount"), int)
                          and r["objectcount"] > 0]
            findings.append(f"- Terms with `objectcount > 0` in page 1: **{len(with_count)}**\n")

            # Label/name length stats — important for the 32-char identifier slot.
            # HAM may use 'name' for some axes and 'culture'/etc for others.
            for label_field in ("name", "century", "culture", "period", "place",
                                "technique", "worktype", "medium", "color",
                                "displayname", "title"):
                labels = [r.get(label_field) for r in recs if isinstance(r.get(label_field), str)]
                if labels:
                    lens = [len(s) for s in labels]
                    over_32 = sum(1 for x in lens if x > 32)
                    findings.append(
                        f"- Label field `{label_field}`: {len(labels)} populated, "
                        f"max len={max(lens)}, >32 chars: {over_32}/{len(labels)}\n"
                    )
                    break

            # Identifier-on-the-wire: most HAM axes use integer ids, person uses personid, etc.
            id_field = None
            for cand in (f"{axis}id", "id", "objectid"):
                if cand in sample:
                    id_field = cand
                    break
            findings.append(f"- Term id field: `{id_field}`\n")

            # Probe top-3 image-bearing counts
            findings.append(f"\n**Top-3 most-populous terms — image-bearing probe:**\n\n")
            findings.append("| # | id | label | objectcount | hasimage=1 count |\n")
            findings.append("|---|---|---|---|---|\n")
            top3 = sorted(
                with_count,
                key=lambda r: r.get("objectcount", 0),
                reverse=True,
            )[:3]
            for i, t in enumerate(top3, 1):
                tid = t.get(id_field) if id_field else None
                label = (t.get("name") or t.get(label_field) or "?")[:60]
                oc = t.get("objectcount", 0)
                img_count = probe_term_image_count(axis, tid) if tid is not None else None
                findings.append(f"| {i} | {tid} | `{label}` | {oc} | "
                                f"{img_count if img_count is not None else 'n/a'} |\n")
                if img_count is not None and oc > 0:
                    summary_rows.append({
                        "axis": axis,
                        "term_id": tid,
                        "label": label,
                        "objectcount": oc,
                        "image_count": img_count,
                        "image_fraction": img_count / oc if oc else 0,
                    })

            findings.append("\n")
        else:
            findings.append("- (no terms returned)\n\n")

    # ------------------------------------------------------------------
    # Recommendation pass
    # ------------------------------------------------------------------
    findings.append("## Axis ranking & recommendation\n\n")
    findings.append(
        "Ranking signals:\n\n"
        "1. **objectcount populated** — required so the browse UI can rank terms "
        "   without per-term probes during enumeration.\n"
        "2. **Term labels ≤ 32 chars** — required for the 33-byte identifier slot. "
        "   When axis terms exceed this on average, the editor would have to filter "
        "   them out, shrinking the usable surface.\n"
        "3. **Image-bearing fraction > 10%** — terms where most artworks lack images "
        "   waste pagination budget on records the refresh has to skip.\n"
        "4. **Single-page enumeration** — total terms ≤ 100 lets the browse modal "
        "   render the picker without paginating.\n\n"
        "*Selection rationale and the final shipping recommendation are written "
        "into REPORT.md after this script's data is combined with the deep-offset "
        "and image probes.*\n"
    )

    REPORT_PATH.write_text("".join(findings), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
