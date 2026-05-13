"""LoC API focused probe — confirms the answers we need for design.

Specific questions this probe answers:

  Q1. Which `fa=` facet-key syntax filters: `original-format:` or
      `original_format:`? (Basic probe showed underscore works.)
  Q2. What's the IIIF-id length distribution per art-relevant format, and
      what % fit within our 48-char `iiif_key` slot?
  Q3. What's the full list of `original_format` facet values returned by
      a generic search response, with their item counts?
  Q4. Can we fetch `info.json` for a real IIIF id, and what sizes does it
      expose?
  Q5. Does the `partof:` facet work for collection-slug filtering, and is
      it a usable axis for the UBI?

Includes retries with exponential backoff because the LoC search API is
slow under load.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any, Optional
from urllib.parse import urlencode

import requests

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
RAW_DIR = OUTPUT_DIR / "raw"
RAW_DIR.mkdir(parents=True, exist_ok=True)
REPORT_PATH = OUTPUT_DIR / "focused.md"

API_ROOT = "https://www.loc.gov"
IIIF_PREFIX = "https://tile.loc.gov/image-services/iiif/"

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev; testing LoC integration)",
    "Accept": "application/json",
})


def fetch_json(url: str, *, tag: str, attempts: int = 3, timeout: int = 60) -> tuple[int, Optional[dict]]:
    delays = [0, 5, 15]
    for i in range(attempts):
        if delays[i]:
            time.sleep(delays[i])
        print(f"[{tag}] GET {url} (attempt {i+1}/{attempts})")
        try:
            r = SESSION.get(url, timeout=timeout)
            print(f"[{tag}]   status={r.status_code} bytes={len(r.content)}")
            try:
                parsed = r.json()
                (RAW_DIR / f"{tag}.json").write_text(
                    json.dumps(parsed, indent=2)[:1_500_000], encoding="utf-8"
                )
                return r.status_code, parsed
            except Exception:
                (RAW_DIR / f"{tag}.json").write_text(
                    r.text[:200_000], encoding="utf-8", errors="replace"
                )
                return r.status_code, None
        except Exception as e:
            print(f"[{tag}]   exception: {e}")
            continue
    return 0, None


def extract_iiif_id(image_url: str) -> Optional[str]:
    if not isinstance(image_url, str) or not image_url.startswith(IIIF_PREFIX):
        return None
    rest = image_url[len(IIIF_PREFIX):]
    for marker in ("/full/", "/info.json", "/square/", "#"):
        idx = rest.find(marker)
        if idx >= 0:
            return rest[:idx]
    return rest


def pick_iiif_from_result(result: dict) -> Optional[str]:
    img_urls = result.get("image_url") or []
    if isinstance(img_urls, str):
        img_urls = [img_urls]
    for u in img_urls:
        iid = extract_iiif_id(u)
        if iid:
            return iid
    resources = result.get("resources") or []
    if isinstance(resources, list):
        for r in resources:
            if not isinstance(r, dict):
                continue
            for key in ("image", "image_url"):
                iid = extract_iiif_id(r.get(key) or "")
                if iid:
                    return iid
    return None


# ---------------------------------------------------------------------------
# Q1 — verify facet key syntax (confirmation only)
# ---------------------------------------------------------------------------

def q1_verify_facet_syntax() -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in ("original-format", "original_format"):
        url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 5, 'sp': 1, 'fa': f'{key}:photo'})}"
        status, j = fetch_json(url, tag=f"Q1_{key}_photo")
        if not j:
            out[key] = {"status": status, "error": True}
            continue
        pag = j.get("pagination") or {}
        out[key] = {
            "status": status,
            "total": pag.get("total"),
            "returned": len(j.get("results") or []),
        }
    return out


# ---------------------------------------------------------------------------
# Q2 — IIIF-id length distribution per format
# ---------------------------------------------------------------------------

ART_RELEVANT_FORMATS = [
    "photo",
    "print",
    "fine art",
    "drawing",
    "painting",
    "poster",
    "map",
]


def q2_iiif_length_distribution() -> dict[str, Any]:
    out: dict[str, Any] = {}
    for fmt in ART_RELEVANT_FORMATS:
        safe_tag = "Q2_" + fmt.replace(" ", "_").replace("/", "_")
        url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 100, 'sp': 1, 'fa': f'original_format:{fmt}'})}"
        status, j = fetch_json(url, tag=safe_tag)
        if not j:
            out[fmt] = {"status": status, "error": True}
            continue
        results = j.get("results") or []
        total = (j.get("pagination") or {}).get("total")
        lengths = []
        no_iiif = 0
        sample_ids: list[tuple[int, str]] = []
        for res in results:
            iid = pick_iiif_from_result(res)
            if iid:
                lengths.append(len(iid))
                sample_ids.append((len(iid), iid))
            else:
                no_iiif += 1
        sample_ids.sort()
        report: dict[str, Any] = {
            "total": total,
            "returned": len(results),
            "iiif_count": len(lengths),
            "no_iiif": no_iiif,
        }
        if lengths:
            lengths.sort()
            report["len_min"] = lengths[0]
            report["len_p25"] = lengths[len(lengths) // 4]
            report["len_p50"] = lengths[len(lengths) // 2]
            report["len_p75"] = lengths[(3 * len(lengths)) // 4]
            report["len_p95"] = lengths[int(0.95 * len(lengths))]
            report["len_max"] = lengths[-1]
            report["fit_48"] = sum(1 for l in lengths if l < 48)
            report["fit_48_pct"] = round(100 * report["fit_48"] / len(lengths), 1)
            report["shortest_3"] = sample_ids[:3]
            report["longest_3"] = sample_ids[-3:]
        out[fmt] = report
    return out


# ---------------------------------------------------------------------------
# Q3 — facet enumeration: list all `original_format` values and counts.
# ---------------------------------------------------------------------------

def q3_facet_enumeration() -> dict[str, Any]:
    url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 1, 'sp': 1})}"
    status, j = fetch_json(url, tag="Q3_facets")
    if not j:
        return {"status": status, "error": True}
    facets = j.get("facets") or []
    out: dict[str, Any] = {"status": status, "facet_titles": []}
    for f in facets:
        name = f.get("name") or f.get("title") or "?"
        out["facet_titles"].append(name)
    # Walk each facet block, capture its top buckets.
    for f in facets:
        name = f.get("name") or "?"
        filters = f.get("filters") or []
        terms = []
        for t in filters[:50]:
            terms.append({
                "title": t.get("title"),
                "count": t.get("count"),
                "term": t.get("term"),
                "id": t.get("id"),
            })
        out[name] = terms
    return out


# ---------------------------------------------------------------------------
# Q4 — info.json fetch for a real IIIF id (we need an art-bearing one).
# ---------------------------------------------------------------------------

def q4_info_json(sample_iids: list[str]) -> list[dict[str, Any]]:
    """Pull info.json for a handful of discovered IIIF ids."""
    results = []
    for i, iid in enumerate(sample_iids[:5]):
        url = f"{IIIF_PREFIX}{iid}/info.json"
        status, j = fetch_json(url, tag=f"Q4_info_{i}", timeout=30)
        item = {"iiif_id": iid, "id_len": len(iid), "status": status, "url": url}
        if j:
            item["@context"] = j.get("@context")
            item["@id"] = j.get("@id")
            item["protocol"] = j.get("protocol")
            item["width"] = j.get("width")
            item["height"] = j.get("height")
            sizes = j.get("sizes") or []
            item["sizes_count"] = len(sizes)
            item["sizes_sample"] = sizes[:8]
            profile = j.get("profile") or []
            if isinstance(profile, list):
                item["profile_summary"] = profile[:2]
        results.append(item)
    return results


# ---------------------------------------------------------------------------
# Q5 — partof facet for collection-slug filtering.
# ---------------------------------------------------------------------------

def q5_partof(slugs: list[str]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for slug in slugs:
        url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 50, 'sp': 1, 'fa': f'partof:{slug}'})}"
        status, j = fetch_json(url, tag=f"Q5_partof_{slug.replace('/', '_')}")
        if not j:
            out[slug] = {"status": status, "error": True}
            continue
        results = j.get("results") or []
        pag = j.get("pagination") or {}
        lengths = [len(iid) for iid in (pick_iiif_from_result(r) for r in results) if iid]
        report: dict[str, Any] = {
            "total": pag.get("total"),
            "returned": len(results),
            "iiif_count": len(lengths),
        }
        if lengths:
            lengths.sort()
            report["len_min"] = lengths[0]
            report["len_p50"] = lengths[len(lengths) // 2]
            report["len_max"] = lengths[-1]
            report["fit_48"] = sum(1 for l in lengths if l < 48)
            report["fit_48_pct"] = round(100 * report["fit_48"] / len(lengths), 1)
        out[slug] = report
    return out


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    findings: dict[str, Any] = {}

    findings["q1_facet_syntax"] = q1_verify_facet_syntax()
    findings["q2_iiif_lengths"] = q2_iiif_length_distribution()
    findings["q3_facet_enumeration"] = q3_facet_enumeration()

    # Pull a handful of sample IIIF ids that we know fit.
    short_ids: list[str] = []
    for fmt, data in findings["q2_iiif_lengths"].items():
        if isinstance(data, dict) and data.get("shortest_3"):
            for length, iid in data["shortest_3"][:1]:
                if iid not in short_ids and length < 48:
                    short_ids.append(iid)
    findings["q4_info_json"] = q4_info_json(short_ids)

    findings["q5_partof"] = q5_partof([
        "civil-war-maps",
        "baseball-cards",
        "panoramic-maps",
        "panoramic-photographs",
        "free-to-use",
    ])

    REPORT_PATH.write_text(json.dumps(findings, indent=2), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
