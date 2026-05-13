"""LoC API — derive IIIF id from storage-services URL.

Hypothesis: items in Prints & Photographs Division surface only a 150px
thumbnail in `image_url`, but the IIIF identifier is derivable. Pattern:

  storage_url:   https://tile.loc.gov/storage-services/service/pnp/hlb/00000/00055_150px.jpg
  iiif_id:       service:pnp:hlb:00000:00055
  iiif_full_url: https://tile.loc.gov/image-services/iiif/service:pnp:hlb:00000:00055/full/!720,720/0/default.jpg

We test this against a handful of PPOC items.
"""

from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path
from typing import Optional
from urllib.parse import urlencode

import requests

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
RAW_DIR = OUTPUT_DIR / "raw"
RAW_DIR.mkdir(parents=True, exist_ok=True)

API_ROOT = "https://www.loc.gov"
IIIF_PREFIX = "https://tile.loc.gov/image-services/iiif/"
STORAGE_PREFIX = "https://tile.loc.gov/storage-services/service/"

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev)",
    "Accept": "application/json",
})


def derive_iiif_id(storage_url: str) -> Optional[str]:
    """Transform an LoC storage-services URL into a IIIF identifier.

    Strips _150px / _250px / etc. suffixes and the file extension, and
    converts /service/x/y/z/foo to service:x:y:z:foo.
    """
    if not isinstance(storage_url, str) or not storage_url.startswith(STORAGE_PREFIX):
        return None
    rest = storage_url[len(STORAGE_PREFIX):]
    # Strip query string and URL fragment.
    rest = rest.split("?", 1)[0].split("#", 1)[0]
    # Strip _150px.jpg, _240px.gif, t.gif, etc. — anything after the last
    # underscore + digits suffix or a trailing 't.gif' / 'r.jpg' size marker.
    # Pattern observations from earlier probes:
    #   /service/pnp/hlb/00000/00055_150px.jpg  -> service:pnp:hlb:00000:00055
    #   /service/pnp/bbc/0000/0000/0001f_150px.jpg -> service:pnp:bbc:0000:0000:0001f
    #   /service/pnp/bbc/0000/0000/0001ft.gif -> service:pnp:bbc:0000:0000:0001f? or :0001ft?
    # We start with the most conservative: strip a final segment matching
    # `(.+?)_(\d+px)\.(jpg|gif|png|tif|tiff)` or `(.+?)\.(jpg|gif|png|tif|tiff)`.
    m = re.match(r"^(.+?)_\d+px\.(?:jpg|gif|png|tif|tiff)$", rest, re.IGNORECASE)
    if m:
        rest = m.group(1)
    else:
        m = re.match(r"^(.+?)\.(?:jpg|gif|png|tif|tiff)$", rest, re.IGNORECASE)
        if m:
            rest = m.group(1)
    parts = rest.split("/")
    if not parts:
        return None
    return "service:" + ":".join(parts)


def fetch(url: str, tag: str, timeout: int = 30) -> tuple[int, Optional[dict]]:
    print(f"[{tag}] GET {url}")
    try:
        r = SESSION.get(url, timeout=timeout)
        try:
            j = r.json()
            (RAW_DIR / f"{tag}.json").write_text(
                json.dumps(j, indent=2)[:1_000_000], encoding="utf-8"
            )
            return r.status_code, j
        except Exception:
            (RAW_DIR / f"{tag}.json").write_text(
                r.text[:100_000], encoding="utf-8", errors="replace"
            )
            return r.status_code, None
    except Exception as e:
        print(f"[{tag}]   exception: {e}")
        return 0, None


def main() -> int:
    # Pull a PPOC page to harvest storage URLs.
    url = f"{API_ROOT}/search/?{urlencode([('fo', 'json'), ('c', 25), ('sp', 1),
                                            ('fa', 'partof:prints and photographs division'),
                                            ('fa', 'online-format:image')])}"
    status, j = fetch(url, "D_ppoc_harvest")
    findings = {"harvest_status": status}
    if not j:
        print("Failed to harvest PPOC results")
        return 1

    results = j.get("results") or []
    print(f"Harvested {len(results)} PPOC results")

    derived: list[dict] = []
    for r in results[:15]:
        # Pull storage URL from image_url[0] and from resources[0].image
        candidates = []
        for u in (r.get("image_url") or []):
            if isinstance(u, str) and u.startswith(STORAGE_PREFIX):
                candidates.append(u)
        for res in (r.get("resources") or []):
            if isinstance(res, dict):
                img = res.get("image") or ""
                if img.startswith(STORAGE_PREFIX):
                    candidates.append(img)
        candidates = list(dict.fromkeys(candidates))  # dedupe
        for c in candidates[:1]:
            iid = derive_iiif_id(c)
            derived.append({
                "storage_url": c,
                "derived_iiif_id": iid,
                "id_len": len(iid) if iid else None,
                "title": r.get("title", "")[:60],
            })

    print("\n--- derived ids ---")
    for d in derived:
        print(f"  {d['derived_iiif_id']:<60} (len={d['id_len']:2}) {d['title']!r}")

    findings["derived"] = derived

    # Test each derived ID against tile.loc.gov info.json
    print("\n--- probe info.json ---")
    test_results = []
    for i, d in enumerate(derived[:8]):
        iid = d.get("derived_iiif_id")
        if not iid:
            continue
        info_url = f"{IIIF_PREFIX}{iid}/info.json"
        status, j = fetch(info_url, f"D_info_{i}")
        item = {"iiif_id": iid, "id_len": len(iid), "info_status": status}
        if j:
            item["width"] = j.get("width")
            item["height"] = j.get("height")
            item["@id"] = j.get("@id")
        # Also test the 720px fetch
        test_url = f"{IIIF_PREFIX}{iid}/full/!720,720/0/default.jpg"
        try:
            rr = SESSION.get(test_url, timeout=30, stream=True)
            head = rr.raw.read(16) if rr.status_code == 200 else b""
            item["test_720_status"] = rr.status_code
            item["test_720_magic"] = head[:4].hex() if head else ""
            rr.close()
        except Exception as e:
            item["test_720_error"] = str(e)
        test_results.append(item)
        time.sleep(0.5)

    print("\n--- test results ---")
    for t in test_results:
        ok = t.get("info_status") == 200 and t.get("test_720_status") == 200
        flag = "OK" if ok else "FAIL"
        print(f"  [{flag}] {t.get('iiif_id'):<50} info={t.get('info_status')} 720={t.get('test_720_status')} dims={t.get('width')}x{t.get('height')}")

    findings["test_results"] = test_results

    # Also test for storage URLs with multiple segments (deep paths).
    print("\n--- length analysis on derived ids ---")
    lengths = [d.get("id_len") for d in derived if d.get("id_len")]
    if lengths:
        fit_48 = sum(1 for l in lengths if l < 48)
        print(f"  count={len(lengths)} min={min(lengths)} max={max(lengths)} median={sorted(lengths)[len(lengths)//2]}")
        print(f"  fit_48: {fit_48}/{len(lengths)} = {round(100*fit_48/len(lengths), 1)}%")
    findings["length_analysis"] = {
        "count": len(lengths),
        "min": min(lengths) if lengths else 0,
        "max": max(lengths) if lengths else 0,
        "fit_48": sum(1 for l in lengths if l < 48),
        "fit_48_pct": round(100 * sum(1 for l in lengths if l < 48) / len(lengths), 1) if lengths else 0,
    }

    (OUTPUT_DIR / "iiif_derive.md").write_text(json.dumps(findings, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
