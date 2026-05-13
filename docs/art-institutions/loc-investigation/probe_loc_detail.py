"""Library of Congress — detailed quirks probe.

Builds on probe_loc_basic.py. Goals:

  1. Characterize IIIF identifier length across formats. Newspaper IDs are
     ~99 chars (won't fit our 48-char iiif_key slot); map IDs are ~38 chars
     (fit). We need to find facet/format combos where 95%+ of IDs fit, so
     refresh can use that as a filter.
  2. Figure out the correct facet-filter syntax. The basic probe found that
     `fa=original_format:photo` produced the *same* first result for
     photo / print / fine art — suggesting the filter wasn't applied or
     the API has multiple competing param styles.
  3. Test the IIIF `info.json` endpoint on a real, image-bearing IIIF
     identifier discovered from the listing response.
  4. Sample multiple "art-relevant" formats (photo, print, fine art,
     drawing, map, poster, painting) to confirm which return IIIF-bearing
     image results.
  5. Investigate facet listing. Where does LoC expose the list of
     `original_format` values for each search context?

Writes JSON output to output/detail.md and per-probe raw dumps in
output/raw/.
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
OUTPUT_DIR.mkdir(exist_ok=True)
REPORT_PATH = OUTPUT_DIR / "detail.md"
RAW_DIR = OUTPUT_DIR / "raw"
RAW_DIR.mkdir(exist_ok=True)

API_ROOT = "https://www.loc.gov"
IIIF_PREFIX = "https://tile.loc.gov/image-services/iiif/"

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev; testing LoC integration)",
    "Accept": "application/json",
})


def _get_json(url: str, *, tag: str, sleep_after: float = 0.4) -> tuple[int, Optional[dict]]:
    print(f"[{tag}] GET {url}")
    r = SESSION.get(url, timeout=30)
    print(f"[{tag}]   status={r.status_code} bytes={len(r.content)}")
    try:
        parsed = r.json()
        (RAW_DIR / f"{tag}.json").write_text(json.dumps(parsed, indent=2)[:2_000_000], encoding="utf-8")
        time.sleep(sleep_after)
        return r.status_code, parsed
    except Exception:
        (RAW_DIR / f"{tag}.json").write_text(r.text[:200_000], encoding="utf-8", errors="replace")
        time.sleep(sleep_after)
        return r.status_code, None


def extract_iiif_id(image_url: str) -> Optional[str]:
    """Extract the IIIF identifier from a tile.loc.gov image URL.

    Patterns we accept:
      https://tile.loc.gov/image-services/iiif/{id}/full/pct:25/0/default.jpg
      https://tile.loc.gov/image-services/iiif/{id}/info.json
    """
    if not isinstance(image_url, str):
        return None
    if not image_url.startswith(IIIF_PREFIX):
        return None
    rest = image_url[len(IIIF_PREFIX):]
    # Strip the IIIF region/size/rotation/quality segments.
    for marker in ("/full/", "/info.json", "/square/", "/info"):
        idx = rest.find(marker)
        if idx >= 0:
            return rest[:idx]
    return rest


def pick_iiif_from_result(result: dict) -> Optional[str]:
    """Walk image_url and resources for a tile.loc.gov IIIF identifier."""
    img_urls = result.get("image_url") or []
    if isinstance(img_urls, str):
        img_urls = [img_urls]
    for u in img_urls:
        iid = extract_iiif_id(u)
        if iid:
            return iid
    # Also check resources[].image (often a IIIF URL too).
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
# Probe A — characterize IIIF id length distribution per `original_format`.
# We pull 100 results for each format and tally how many have IIIF ids
# at each length bucket.
# ---------------------------------------------------------------------------

CANDIDATE_FORMATS = [
    "photo",
    "print",
    "fine art",
    "drawing",
    "painting",
    "poster",
    "map",
    "manuscript/mixed material",
    "film, video",
    "online resource",
    "book",
    "newspaper",
]


def probe_format(fmt: str, tag: str, page_size: int = 100) -> dict[str, Any]:
    """Pull `page_size` results filtered by original_format and inspect them."""
    # The LoC docs show `fa=` is the accepted facet-filter param, key as
    # `original-format` (with a dash). Many community examples also use the
    # underscored form. We'll try both and see which restricts results.
    out: dict[str, Any] = {"format": fmt}
    for facet_key in ("original-format", "original_format"):
        params = {
            "fo": "json",
            "c": page_size,
            "sp": 1,
            "fa": f"{facet_key}:{fmt}",
        }
        url = f"{API_ROOT}/search/?{urlencode(params)}"
        status, j = _get_json(url, tag=f"{tag}_{facet_key}")
        if not j:
            out[f"{facet_key}_status"] = status
            out[f"{facet_key}_error"] = True
            continue
        pagination = j.get("pagination") or {}
        results = j.get("results") or []
        total = pagination.get("total", 0)
        out[f"{facet_key}_status"] = status
        out[f"{facet_key}_total"] = total
        out[f"{facet_key}_returned"] = len(results)
        # Sample each result for IIIF id length and original_format values
        lengths: list[int] = []
        formats_observed: dict[str, int] = {}
        no_iiif = 0
        for res in results:
            iid = pick_iiif_from_result(res)
            if iid:
                lengths.append(len(iid))
            else:
                no_iiif += 1
            fmts = res.get("original_format") or []
            if isinstance(fmts, str):
                fmts = [fmts]
            for f in fmts:
                formats_observed[f] = formats_observed.get(f, 0) + 1
        # Summarize length distribution.
        if lengths:
            sorted_lens = sorted(lengths)
            out[f"{facet_key}_iiif_count"] = len(lengths)
            out[f"{facet_key}_iiif_no_iiif"] = no_iiif
            out[f"{facet_key}_iiif_len_min"] = sorted_lens[0]
            out[f"{facet_key}_iiif_len_p50"] = sorted_lens[len(sorted_lens) // 2]
            out[f"{facet_key}_iiif_len_p95"] = sorted_lens[int(len(sorted_lens) * 0.95)]
            out[f"{facet_key}_iiif_len_max"] = sorted_lens[-1]
            out[f"{facet_key}_iiif_fit_48"] = sum(1 for l in lengths if l < 48)
            out[f"{facet_key}_iiif_fit_48_pct"] = round(
                100 * out[f"{facet_key}_iiif_fit_48"] / len(lengths), 1
            )
        else:
            out[f"{facet_key}_iiif_count"] = 0
            out[f"{facet_key}_iiif_no_iiif"] = no_iiif
        out[f"{facet_key}_top_formats"] = sorted(
            formats_observed.items(), key=lambda kv: -kv[1]
        )[:5]
    return out


# ---------------------------------------------------------------------------
# Probe B — facet enumeration: discover the set of original_format values.
# Use the "facets" key of a top-level search response.
# ---------------------------------------------------------------------------

def probe_facets_enumeration() -> dict[str, Any]:
    url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 1, 'sp': 1})}"
    status, j = _get_json(url, tag="B_facets_enumeration")
    out: dict[str, Any] = {"status": status, "url": url}
    if not j:
        return out
    facets = j.get("facets") or []
    out["facet_titles"] = [f.get("title") or f.get("name") or "?" for f in facets]
    for f in facets:
        name = f.get("name") or f.get("title") or "?"
        filters = f.get("filters") or []
        out[f"{name}_top_terms"] = [
            {"title": t.get("title"), "count": t.get("count"), "term": t.get("term")}
            for t in filters[:25]
        ]
    return out


# ---------------------------------------------------------------------------
# Probe C — IIIF info.json reachability. Pick a real IIIF id from a search,
# fetch info.json, parse it.
# ---------------------------------------------------------------------------

def probe_info_json(iiif_id: str, tag: str) -> dict[str, Any]:
    url = f"{IIIF_PREFIX}{iiif_id}/info.json"
    status, j = _get_json(url, tag=tag)
    out: dict[str, Any] = {"status": status, "url": url, "iiif_id": iiif_id, "id_len": len(iiif_id)}
    if not j:
        return out
    out["@context"] = j.get("@context")
    out["@id"] = j.get("@id")
    out["protocol"] = j.get("protocol")
    out["width"] = j.get("width")
    out["height"] = j.get("height")
    sizes = j.get("sizes") or []
    out["sizes_count"] = len(sizes)
    out["sizes_sample"] = sizes[:4]
    profile = j.get("profile") or []
    if isinstance(profile, list):
        out["profile_sample"] = profile[:2]
    else:
        out["profile_sample"] = profile
    return out


# ---------------------------------------------------------------------------
# Probe D — does &fa=partof:<collection-slug> work?
# Some collections may be more reliably scoped via partof.
# ---------------------------------------------------------------------------

def probe_partof(slug: str, tag: str) -> dict[str, Any]:
    params = {"fo": "json", "c": 25, "sp": 1, "fa": f"partof:{slug}"}
    url = f"{API_ROOT}/search/?{urlencode(params)}"
    status, j = _get_json(url, tag=tag)
    out: dict[str, Any] = {"status": status, "url": url, "slug": slug}
    if not j:
        return out
    pagination = j.get("pagination") or {}
    out["total"] = pagination.get("total")
    results = j.get("results") or []
    out["returned"] = len(results)
    lengths = []
    for res in results:
        iid = pick_iiif_from_result(res)
        if iid:
            lengths.append(len(iid))
    if lengths:
        out["iiif_len_min"] = min(lengths)
        out["iiif_len_max"] = max(lengths)
        out["iiif_count"] = len(lengths)
    return out


# ---------------------------------------------------------------------------
# Probe E — collection listings: get an inventory of every collection with
# its item count. Use /collections/?c=160 with multiple pages.
# ---------------------------------------------------------------------------

def probe_all_collections(max_pages: int = 5) -> dict[str, Any]:
    inventory: list[dict[str, Any]] = []
    for sp in range(1, max_pages + 1):
        url = f"{API_ROOT}/collections/?{urlencode({'fo': 'json', 'c': 160, 'sp': sp})}"
        status, j = _get_json(url, tag=f"E_collections_p{sp}")
        if not j:
            break
        results = j.get("results") or []
        for c in results:
            inventory.append({
                "title": c.get("title"),
                "url": c.get("url"),
                "count": c.get("count") or c.get("number_of_items"),
                "id": c.get("id"),
            })
        pagination = j.get("pagination") or {}
        if not pagination.get("next"):
            break
    return {"collection_count": len(inventory), "first_50": inventory[:50]}


# ---------------------------------------------------------------------------
# Probe F — direct hits on /resource/ endpoints (used by the resource_link
# in search results) for one IIIF item.
# ---------------------------------------------------------------------------

def probe_resource(resource_url_or_slug: str, tag: str) -> dict[str, Any]:
    if resource_url_or_slug.startswith("http"):
        url = resource_url_or_slug + ("&" if "?" in resource_url_or_slug else "?") + "fo=json"
    else:
        url = f"{API_ROOT}/resource/{resource_url_or_slug}/?fo=json"
    status, j = _get_json(url, tag=tag)
    out: dict[str, Any] = {"status": status, "url": url}
    if not j:
        return out
    out["top_keys"] = sorted(j.keys())[:30]
    resources = j.get("resources") or []
    out["resources_count"] = len(resources)
    if resources:
        r0 = resources[0]
        out["r0_keys"] = sorted(r0.keys())
        out["r0_url"] = r0.get("url")
        out["r0_image"] = r0.get("image")
    return out


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    findings: dict[str, Any] = {}

    # A — format distribution.
    findings["formats"] = {}
    for fmt in CANDIDATE_FORMATS:
        safe_tag = "A_" + fmt.replace(" ", "_").replace("/", "_").replace(",", "_")
        findings["formats"][fmt] = probe_format(fmt, safe_tag)

    # B — facet enumeration.
    findings["facets"] = probe_facets_enumeration()

    # C — info.json for a few discovered IIIF ids. We pick the shortest
    # IIIF id we saw under "fine art" / "photo" / "print" formats.
    sample_ids: list[str] = []
    for fmt, data in findings["formats"].items():
        # Use any of the two facet-key variants that returned valid sample data.
        for prefix in ("original-format", "original_format"):
            sample_url = data.get(f"{prefix}_iiif_count")
            if not sample_url:
                continue
            # We need a real id — re-issue a small fetch.
        # We already saved raw dumps; instead, do one targeted re-issue.
    # Targeted re-issue: fetch fine art, take first IIIF id.
    for fmt in ["fine art", "photo", "print", "drawing"]:
        for facet_key in ("original-format", "original_format"):
            url = f"{API_ROOT}/search/?{urlencode({'fo': 'json', 'c': 10, 'sp': 1, 'fa': f'{facet_key}:{fmt}'})}"
            r = SESSION.get(url, timeout=30)
            time.sleep(0.4)
            try:
                j = r.json()
            except Exception:
                continue
            for res in (j.get("results") or []):
                iid = pick_iiif_from_result(res)
                if iid and len(iid) < 48:
                    sample_ids.append((fmt, facet_key, iid))
                    break
            if any(s[0] == fmt for s in sample_ids):
                break

    findings["info_samples"] = []
    for idx, (fmt, facet_key, iid) in enumerate(sample_ids[:6]):
        findings["info_samples"].append({
            "format": fmt,
            "facet_key": facet_key,
            "iiif_id": iid,
            "info": probe_info_json(iid, tag=f"C_info_{idx}"),
        })

    # D — partof probes
    findings["partof"] = {
        "civil-war-maps": probe_partof("civil-war-maps", "D_partof_civil_war_maps"),
        "baseball-cards": probe_partof("baseball-cards", "D_partof_baseball_cards"),
    }

    # E — collection inventory (first 5 pages of 160 each = up to 800 collections)
    findings["collection_inventory"] = probe_all_collections(max_pages=5)

    REPORT_PATH.write_text(json.dumps(findings, indent=2), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
