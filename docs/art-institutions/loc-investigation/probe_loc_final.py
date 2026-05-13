"""LoC API — final, definitive probe.

The facet probe revealed the correct syntax:

  - Field name: hyphenated (e.g. `original-format`, `online-format`, `partof`)
  - Value: lowercase display title with spaces (e.g. `photo, print, drawing`,
    `prints and photographs division`, `civil war maps`)
  - `fa=` parameter or `/photos/` / `/maps/` / `/collections/{slug}/` URL paths
    are equivalent — both rewrite to the same canonical query.

This probe nails down:

  Q-FINAL.1 With proper filter, what's the IIIF-id length distribution and
            48-char fit %? We pull 100 items each for several format/partof
            combinations.
  Q-FINAL.2 What format values does LoC's `Original Format` facet enumerate?
  Q-FINAL.3 Does `info.json` parse cleanly? What sizes does LoC's IIIF expose?
  Q-FINAL.4 Pulling /collections/ index — get the full list of collection
            slugs + titles + counts.
  Q-FINAL.5 Verify that combining a partof + online-format:image yields
            mostly image-bearing IIIF items.
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
REPORT_PATH = OUTPUT_DIR / "final.md"

API_ROOT = "https://www.loc.gov"
IIIF_PREFIX = "https://tile.loc.gov/image-services/iiif/"

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev)",
    "Accept": "application/json",
})


def fetch(url: str, tag: str, attempts: int = 3) -> tuple[int, Optional[dict]]:
    delays = [0, 5, 15]
    for i in range(attempts):
        if delays[i]:
            time.sleep(delays[i])
        print(f"[{tag}] GET {url} (attempt {i+1}/{attempts})")
        try:
            r = SESSION.get(url, timeout=60)
            try:
                j = r.json()
                (RAW_DIR / f"{tag}.json").write_text(
                    json.dumps(j, indent=2)[:1_500_000], encoding="utf-8"
                )
                return r.status_code, j
            except Exception:
                (RAW_DIR / f"{tag}.json").write_text(
                    r.text[:200_000], encoding="utf-8", errors="replace"
                )
                return r.status_code, None
        except Exception as e:
            print(f"[{tag}]   exception: {e}")
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


def summarize_iiif_lengths(results: list, label: str) -> dict[str, Any]:
    lengths: list[int] = []
    no_iiif = 0
    samples_short: list[tuple[int, str]] = []
    samples_long: list[tuple[int, str]] = []
    for r in results:
        iid = pick_iiif_from_result(r)
        if iid:
            lengths.append(len(iid))
            if len(samples_short) < 5:
                samples_short.append((len(iid), iid))
            samples_long.append((len(iid), iid))
        else:
            no_iiif += 1
    samples_long.sort(reverse=True)
    samples_short.sort()
    out: dict[str, Any] = {
        "returned": len(results),
        "with_iiif": len(lengths),
        "without_iiif": no_iiif,
    }
    if lengths:
        lengths.sort()
        out["len_min"] = lengths[0]
        out["len_p25"] = lengths[len(lengths) // 4]
        out["len_p50"] = lengths[len(lengths) // 2]
        out["len_p75"] = lengths[3 * len(lengths) // 4]
        out["len_p95"] = lengths[int(0.95 * len(lengths))]
        out["len_max"] = lengths[-1]
        out["fit_48"] = sum(1 for l in lengths if l < 48)
        out["fit_48_pct"] = round(100 * out["fit_48"] / len(lengths), 1)
        out["shortest_5"] = samples_short[:5]
        out["longest_5"] = samples_long[:5]
    return out


# ---------------------------------------------------------------------------
# Q-FINAL.1 — IIIF id length distribution with proper filters
# ---------------------------------------------------------------------------

def q_final_1() -> dict[str, Any]:
    """Test each format + division combination. Use both `fa=` and `/path/`
    forms where both exist, to confirm equivalence."""
    out: dict[str, Any] = {}
    test_queries = [
        # (label, params)
        ("photos_path", {"_url": f"{API_ROOT}/photos/?fo=json&c=100&sp=1"}),
        ("maps_path", {"_url": f"{API_ROOT}/maps/?fo=json&c=100&sp=1"}),
        ("pp_division", {"fa": "partof:prints and photographs division"}),
        ("photo_print_drawing_fmt", {"fa": "original-format:photo, print, drawing"}),
        ("manuscript_fmt", {"fa": "original-format:manuscript/mixed material"}),
        ("notated_music_fmt", {"fa": "original-format:notated music"}),
        ("poster_collection", {"_url": f"{API_ROOT}/collections/world-war-i-posters/?fo=json&c=100&sp=1"}),
        ("free_to_use_path", {"_url": f"{API_ROOT}/free-to-use/?fo=json&c=100&sp=1"}),
        # Verify image-bearing
        ("pp_div_image", {"fa1": "partof:prints and photographs division",
                          "fa2": "online-format:image"}),
        ("photo_print_drawing_image", {"fa1": "original-format:photo, print, drawing",
                                       "fa2": "online-format:image"}),
    ]
    for label, q in test_queries:
        if "_url" in q:
            url = q["_url"]
        else:
            params = [("fo", "json"), ("c", "100"), ("sp", "1")]
            if "fa" in q:
                params.append(("fa", q["fa"]))
            if "fa1" in q:
                params.append(("fa", q["fa1"]))
                params.append(("fa", q["fa2"]))
            url = f"{API_ROOT}/search/?{urlencode(params)}"
        status, j = fetch(url, f"F1_{label}")
        if not j:
            out[label] = {"status": status, "error": True}
            continue
        out[label] = {
            "url": url,
            "total": (j.get("pagination") or {}).get("total"),
            "trail": [(t.get("facet"), t.get("value")) for t in (j.get("facet_trail") or [])],
        }
        out[label].update(summarize_iiif_lengths(j.get("results") or [], label))
    return out


# ---------------------------------------------------------------------------
# Q-FINAL.3 — info.json for several IIIF ids
# ---------------------------------------------------------------------------

def q_final_3(iids: list[str]) -> list[dict[str, Any]]:
    results = []
    for i, iid in enumerate(iids[:5]):
        url = f"{IIIF_PREFIX}{iid}/info.json"
        status, j = fetch(url, f"F3_info_{i}")
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
            tiles = j.get("tiles") or []
            item["tiles_sample"] = tiles[:2]
            profile = j.get("profile") or []
            if isinstance(profile, list):
                item["profile_summary"] = profile[:2]
            item["compliance"] = j.get("profile") if isinstance(j.get("profile"), str) else None
            # Try a real 720px fetch
            test_url = f"{IIIF_PREFIX}{iid}/full/!720,720/0/default.jpg"
            try:
                rr = SESSION.get(test_url, timeout=30, stream=True)
                rr.raw.decode_content = True
                # Read just the first 16 bytes to check JPEG magic
                head = rr.raw.read(16) if rr.status_code == 200 else b""
                item["test_720_status"] = rr.status_code
                item["test_720_bytes_sniff"] = head[:4].hex() if head else ""
                rr.close()
            except Exception as e:
                item["test_720_error"] = str(e)
        results.append(item)
        time.sleep(0.5)
    return results


# ---------------------------------------------------------------------------
# Q-FINAL.4 — collections index, fully paginated.
# ---------------------------------------------------------------------------

def q_final_4(max_pages: int = 5) -> dict[str, Any]:
    inventory: list[dict[str, Any]] = []
    page_totals: list[int] = []
    for sp in range(1, max_pages + 1):
        url = f"{API_ROOT}/collections/?{urlencode({'fo': 'json', 'c': 160, 'sp': sp})}"
        status, j = fetch(url, f"F4_coll_p{sp}")
        if not j:
            break
        results = j.get("results") or []
        pag = j.get("pagination") or {}
        page_totals.append(pag.get("total"))
        for c in results:
            url_full = c.get("url") or ""
            # collection slug: extract from URL
            slug = None
            if url_full.startswith(f"{API_ROOT}/collections/"):
                slug = url_full[len(f"{API_ROOT}/collections/"):].strip("/")
            inventory.append({
                "title": c.get("title"),
                "slug": slug,
                "url": url_full,
                "count": c.get("count") or c.get("number_of_items"),
                "id": c.get("id"),
            })
        if not pag.get("next"):
            break
    return {
        "collection_count": len(inventory),
        "page_totals": page_totals,
        "first_30": inventory[:30],
        "all_titles": [c.get("title") for c in inventory],
    }


# ---------------------------------------------------------------------------
# Q-FINAL.5 — Compare image-bearing yield across queries.
# Already covered by Q-FINAL.1 with the `_image` variants.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    findings: dict[str, Any] = {}

    findings["q1_iiif_lengths"] = q_final_1()

    # Pick a few short IIIF ids from results above for the info.json probe.
    short_ids: list[str] = []
    for label, data in findings["q1_iiif_lengths"].items():
        if isinstance(data, dict) and data.get("shortest_5"):
            for length, iid in data["shortest_5"][:1]:
                if iid not in short_ids and length < 48:
                    short_ids.append(iid)
        if len(short_ids) >= 5:
            break

    findings["q3_info_json"] = q_final_3(short_ids)

    findings["q4_collections_index"] = q_final_4(max_pages=5)

    REPORT_PATH.write_text(json.dumps(findings, indent=2), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
