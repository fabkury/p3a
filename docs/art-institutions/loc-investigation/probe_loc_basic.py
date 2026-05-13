"""Library of Congress JSON API + IIIF — basic probes.

The goals here are to characterize, on a real workstation against the live API:

  1. Whether anonymous fetches need any specific headers (User-Agent, Accept,
     etc.) — the museum-art research notes flagged a 403 in an earlier probe.
  2. The JSON envelope shape for a typical search query: which fields tell us
     "this result has a IIIF image we can fetch at 720px?"
  3. The IIIF Image API endpoint pattern actually used by LoC and whether
     `info.json` is reachable anonymously.
  4. The basic pagination scheme (`sp` page number + `c` count per page).
  5. Whether faceting on `original_format=photo,print,...` works for scoping
     to visual art.

We write a Markdown report at output/basic.md plus raw JSON dumps next to
this file (so the design pass can quote concrete shapes).
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
REPORT_PATH = OUTPUT_DIR / "basic.md"
RAW_DIR = OUTPUT_DIR / "raw"
RAW_DIR.mkdir(exist_ok=True)

API_ROOT = "https://www.loc.gov"

# Use a polite User-Agent (the candidate doc suggested 403s may stem from
# fingerprint policies). Mirror the AIC-style identification convention.
SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev; testing LoC integration)",
    "Accept": "application/json",
})


def _get_json(url: str, *, tag: str, sleep_after: float = 0.5) -> tuple[int, Optional[dict]]:
    """Fetch a URL expecting JSON; dump raw to RAW_DIR; return (status, parsed)."""
    print(f"[{tag}] GET {url}")
    r = SESSION.get(url, timeout=30)
    print(f"[{tag}]   status={r.status_code} bytes={len(r.content)} "
          f"content-type={r.headers.get('Content-Type', '')}")
    raw_path = RAW_DIR / f"{tag}.json"
    try:
        # Try to pretty-print if JSON; else write raw text.
        parsed = r.json()
        raw_path.write_text(json.dumps(parsed, indent=2)[:1_000_000], encoding="utf-8")
        time.sleep(sleep_after)
        return r.status_code, parsed
    except Exception:
        raw_path.write_text(r.text[:1_000_000], encoding="utf-8", errors="replace")
        time.sleep(sleep_after)
        return r.status_code, None


# ---------------------------------------------------------------------------
# Probe 1 — site-wide search with fo=json. Tests anonymous fetch + headers.
# ---------------------------------------------------------------------------

def probe_search_root() -> dict[str, Any]:
    """Pull a tiny generic search to confirm the JSON envelope works."""
    params = {"fo": "json", "c": 5, "sp": 1, "q": "art"}
    url = f"{API_ROOT}/search/?{urlencode(params)}"
    status, j = _get_json(url, tag="01_search_root")
    out = {"status": status, "url": url}
    if j is None:
        return out
    out["top_keys"] = sorted(j.keys())[:30]
    out["pagination"] = j.get("pagination", {})
    results = j.get("results") or []
    out["results_count"] = len(results)
    if results:
        sample = results[0]
        out["first_result_keys"] = sorted(sample.keys())
        out["first_result_sample"] = {
            k: sample.get(k) for k in ("id", "title", "image_url", "url",
                                       "original_format", "online_format",
                                       "resources", "mime_type", "type")
        }
    facets = j.get("facets") or []
    out["facet_names"] = [f.get("title") or f.get("name") or "?" for f in facets][:20]
    return out


# ---------------------------------------------------------------------------
# Probe 2 — search restricted to image-bearing items.
# ---------------------------------------------------------------------------

def probe_search_with_format(fmt: str, tag: str) -> dict[str, Any]:
    params = {"fo": "json", "c": 5, "sp": 1, "fa": f"original_format:{fmt}"}
    url = f"{API_ROOT}/search/?{urlencode(params)}"
    status, j = _get_json(url, tag=tag)
    out = {"status": status, "url": url, "format_filter": fmt}
    if j is None:
        return out
    out["pagination"] = j.get("pagination", {})
    results = j.get("results") or []
    out["results_count"] = len(results)
    if results:
        s = results[0]
        out["first_result_keys"] = sorted(s.keys())
        out["first_id"] = s.get("id")
        out["first_image_url"] = s.get("image_url")
        out["first_resources"] = s.get("resources")
        out["first_url"] = s.get("url")
        out["first_original_format"] = s.get("original_format")
        out["first_online_format"] = s.get("online_format")
    return out


# ---------------------------------------------------------------------------
# Probe 3 — collection listing.
# Hypothesis: /collections/{slug}/?fo=json is the per-collection listing.
# ---------------------------------------------------------------------------

def probe_collection(slug: str, tag: str) -> dict[str, Any]:
    url = f"{API_ROOT}/collections/{slug}/?{urlencode({'fo': 'json', 'c': 5, 'sp': 1})}"
    status, j = _get_json(url, tag=tag)
    out = {"status": status, "url": url, "slug": slug}
    if j is None:
        return out
    out["pagination"] = j.get("pagination", {})
    results = j.get("results") or []
    out["results_count"] = len(results)
    out["top_keys"] = sorted(j.keys())[:30]
    if results:
        s = results[0]
        out["first_result_keys"] = sorted(s.keys())
        out["first_image_url"] = s.get("image_url")
        out["first_id"] = s.get("id")
        out["first_url"] = s.get("url")
    return out


# ---------------------------------------------------------------------------
# Probe 4 — facets endpoint: get the list of available top-level collections.
# Hypothesis: format facets / collection facets are returned at /collections/
# or under fo=json on a top-level search.
# ---------------------------------------------------------------------------

def probe_collections_index() -> dict[str, Any]:
    url = f"{API_ROOT}/collections/?{urlencode({'fo': 'json', 'c': 25, 'sp': 1})}"
    status, j = _get_json(url, tag="04_collections_index")
    out = {"status": status, "url": url}
    if j is None:
        return out
    out["top_keys"] = sorted(j.keys())[:30]
    out["pagination"] = j.get("pagination", {})
    results = j.get("results") or []
    out["results_count"] = len(results)
    if results:
        s = results[0]
        out["first_result_keys"] = sorted(s.keys())
        out["first_id"] = s.get("id")
        out["first_url"] = s.get("url")
        out["first_title"] = s.get("title")
        out["first_count"] = s.get("count") or s.get("number_of_items") or s.get("total")
    return out


# ---------------------------------------------------------------------------
# Probe 5 — one item's full JSON. We want to confirm we can pull an item's
# IIIF service URL when needed.
# ---------------------------------------------------------------------------

def probe_item(item_id: str, tag: str) -> dict[str, Any]:
    """item_id can be in the form '2003673974' or a slug — we hit /item/{id}/."""
    url = f"{API_ROOT}/item/{item_id}/?{urlencode({'fo': 'json'})}"
    status, j = _get_json(url, tag=tag)
    out = {"status": status, "url": url, "item_id": item_id}
    if j is None:
        return out
    out["top_keys"] = sorted(j.keys())[:40]
    item = j.get("item") or {}
    resources = j.get("resources") or []
    out["item_top_keys"] = sorted(item.keys())[:60]
    out["resources_count"] = len(resources)
    if resources:
        r0 = resources[0]
        out["resource_keys"] = sorted(r0.keys())
        out["resource_url"] = r0.get("url")
        out["resource_image_url"] = r0.get("image_url") or r0.get("image")
        files = r0.get("files") or []
        out["files_count"] = len(files)
        if files and isinstance(files[0], list):
            # files is typically a list of lists (one per page / segment)
            out["first_file_seg_count"] = len(files[0])
            if files[0]:
                ff = files[0][0]
                if isinstance(ff, dict):
                    out["first_file_keys"] = sorted(ff.keys())
                    out["first_file_url"] = ff.get("url")
                    out["first_file_mimetype"] = ff.get("mimetype")
                    out["first_file_other_url"] = ff.get("other") or ff.get("other_url")
                    out["first_file_info"] = ff.get("info")
    return out


# ---------------------------------------------------------------------------
# Probe 6 — IIIF info.json fetch directly against tile.loc.gov for a known
# image. We pick one from the item probe afterwards.
# ---------------------------------------------------------------------------

def probe_iiif_info(iiif_image_base: str, tag: str) -> dict[str, Any]:
    """Fetch info.json at the given IIIF image base URL."""
    url = iiif_image_base.rstrip("/") + "/info.json"
    status, j = _get_json(url, tag=tag)
    out = {"status": status, "url": url}
    if j is None:
        return out
    out["@context"] = j.get("@context")
    out["@id"] = j.get("@id")
    out["protocol"] = j.get("protocol")
    out["width"] = j.get("width")
    out["height"] = j.get("height")
    profile = j.get("profile") or []
    out["profile_summary"] = (
        profile[:2] if isinstance(profile, list) else profile
    )
    sizes = j.get("sizes") or []
    out["sizes_count"] = len(sizes)
    out["sizes_sample"] = sizes[:6]
    return out


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    findings: dict[str, Any] = {}

    findings["search_root"] = probe_search_root()
    findings["search_photo"] = probe_search_with_format("photo", "02_search_photo")
    findings["search_print"] = probe_search_with_format("print", "02b_search_print")
    findings["search_fine_arts"] = probe_search_with_format(
        "fine art", "02c_search_fine_arts"
    )
    findings["collection_civil_war_maps"] = probe_collection(
        "civil-war-maps", "03_collection_civil_war_maps"
    )
    findings["collection_baseball_cards"] = probe_collection(
        "baseball-cards", "03b_collection_baseball_cards"
    )
    findings["collections_index"] = probe_collections_index()

    # Try a couple of well-known items. 2003673974 is a public-domain print.
    findings["item_2003673974"] = probe_item("2003673974", "05_item_2003673974")

    # IIIF info.json — use a known-good identifier from LoC's tile service.
    # Pattern: https://tile.loc.gov/image-services/iiif/{recipe}/info.json
    # We'll try the canonical tile service base, plus a guess derived from
    # the item probe if it returned a resource URL.
    iiif_base_guess = "https://tile.loc.gov/image-services/iiif/service:pnp:cph:3a40000:3a40000:3a41000:3a41008v"
    findings["iiif_info_guess"] = probe_iiif_info(iiif_base_guess, "06_iiif_info_guess")

    # Pull a resource URL from the item probe if available.
    item_data = findings.get("item_2003673974") or {}
    derived_iiif = item_data.get("resource_image_url") or item_data.get("first_file_url")
    if derived_iiif and isinstance(derived_iiif, str):
        # Strip trailing /default.jpg or path components after IIIF identifier.
        # Try the pattern: ".../iiif/{id}/full/pct:25/0/default.jpg" -> ".../iiif/{id}"
        base = derived_iiif
        for marker in ("/full/", "/info.json"):
            idx = base.find(marker)
            if idx >= 0:
                base = base[:idx]
                break
        findings["iiif_info_derived"] = probe_iiif_info(base, "07_iiif_info_derived")

    # Write the report.
    REPORT_PATH.write_text(json.dumps(findings, indent=2), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
