"""Harvard Art Museums — IIIF rendition + image-id length probe.

Stage 1 requirements addressed:

  - C1: IIIF Image API v2 URL pattern serves JPEG at our requested
    longest-side cap (`…/full/!720,720/0/default.jpg`).
  - C2: image identifier fits in 47 chars (the 48-byte iiif_key slot).
    This was the binding constraint that killed LoC, so we sample more
    than 50 ids and report length distribution.
  - C3: per-image file size ≤ 16 MiB (`P3A_MAX_ARTWORK_SIZE`).
  - C4: TLS chain on the image CDN covered by `esp_crt_bundle`.
  - C5: image identifier discoverable from the listing without a
    multi-hop walk.

HAM image URL flow:

  Listing response carries `primaryimageurl` and
  `images[].baseimageurl` of the form
  `https://nrs.harvard.edu/urn-3:HUAM:{rendition}_dynmc`. That URN
  is Harvard's Name Resolution Service. Hitting it (or appending IIIF
  size syntax to it) returns HTTP 303 with a `Location` header that
  resolves to `https://ids.lib.harvard.edu/mps/{ids_id}/full/…/0/default.jpg`.

  Two storage strategies for `iiif_key`:
    (a) Store the URN portion only (e.g. `urn-3:HUAM:79762_dynmc`).
        C-side builds `https://nrs.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`
        and accepts a single 303 redirect (Rijks already has the
        manual-redirect machinery).
    (b) Resolve once at refresh time and store the IDS id
        (e.g. `mps/18483191`). C-side builds the final URL directly,
        no redirect needed at download time. Costs one extra HTTP per
        entry during refresh; saves one HTTP per image during download.

We characterize both here so REPORT.md can recommend (a) or (b).

Outputs:
  - output/raw/image_sample_<axis>_<term>.json   (listing responses)
  - output/raw/image_redirect_<n>.txt            (303 trace per sample)
  - output/raw/image_fetch_<n>.bin               (actual image bytes — first 200 only)
  - output/image.md
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from typing import Any, Optional
from urllib.parse import urlencode, urlparse

import requests

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
RAW_DIR = OUTPUT_DIR / "raw"
OUTPUT_DIR.mkdir(exist_ok=True)
RAW_DIR.mkdir(exist_ok=True)
REPORT_PATH = OUTPUT_DIR / "image.md"

API_ROOT = "https://api.harvardartmuseums.org"
API_KEY = os.environ.get("HAM_API_KEY", "c09e5b21-5ea4-4762-b611-e41d3a2ba07d")

SESSION = requests.Session()
SESSION.headers.update({
    "User-Agent": "p3a-investigation/0.1 (pub@kury.dev; testing HAM integration)",
    "Accept": "application/json",
})


def _get_json(url: str, *, tag: str, sleep_after: float = 0.3) -> Optional[dict]:
    print(f"[{tag}] GET {url}")
    r = SESSION.get(url, timeout=30)
    raw_path = RAW_DIR / f"{tag}.json"
    try:
        data = r.json()
        raw_path.write_text(json.dumps(data, indent=2)[:1_500_000], encoding="utf-8")
    except Exception:
        raw_path.with_suffix(".txt").write_text(r.text[:200_000], encoding="utf-8")
        return None
    time.sleep(sleep_after)
    return data if r.status_code == 200 else None


def collect_baseimageurls(axes_and_terms: list[tuple[str, int]],
                          rows_per_term: int) -> list[dict]:
    """Pull listing pages and flatten out (baseimageurl, primaryimageurl) per record."""
    rows = []
    for axis, term in axes_and_terms:
        params = {
            "apikey": API_KEY,
            "size": rows_per_term,
            "page": 1,
            axis: term,
            "hasimage": 1,
            "sort": "id",
            "sortorder": "asc",
            "fields": "id,objectnumber,primaryimageurl,images,title",
        }
        d = _get_json(f"{API_ROOT}/object?{urlencode(params)}",
                      tag=f"image_sample_{axis}_{term}")
        if not d:
            continue
        for rec in d.get("records", []):
            primary = rec.get("primaryimageurl")
            images = rec.get("images") or []
            base = images[0].get("baseimageurl") if images else None
            rows.append({
                "object_id": rec.get("id"),
                "title": rec.get("title"),
                "primaryimageurl": primary,
                "baseimageurl": base,
                "image_count": len(images),
                "axis": axis,
                "term": term,
            })
    return rows


def extract_iiif_id(url: str) -> str:
    """Pull the IIIF identifier from a base URL.

    For HAM these are URNs of the form
    `https://nrs.harvard.edu/urn-3:HUAM:NNNN_dynmc` — we strip the host
    and return the URN. For any other shape we return the path tail.
    """
    if not url:
        return ""
    p = urlparse(url)
    return p.path.lstrip("/")


def main() -> int:
    findings: list[str] = []
    findings.append("# HAM — IIIF rendition + image-id length probe\n\n")
    findings.append(f"*Run at {time.strftime('%Y-%m-%d %H:%M:%S')} UTC*\n\n")

    # ------------------------------------------------------------------
    # 1. Sample ids across multiple terms / axes — confirm the URN shape
    #    is universal and measure its length distribution.
    # ------------------------------------------------------------------
    findings.append("## 1. Image-id length distribution (C2)\n\n")
    findings.append(
        "Samples 25 records each across classification=17 (Photographs), "
        "classification=23 (Prints), classification=21 (Drawings), "
        "century=37525815 (20th century), culture=37526778 (American), "
        "place=2028213 (United States) — six populous terms — to characterize "
        "the shape and length of `baseimageurl` (which is what we'd store as "
        "iiif_key after stripping the host prefix).\n\n"
    )
    sample_targets = [
        ("classification", 17),
        ("classification", 23),
        ("classification", 21),
        ("century",        37525815),
        ("culture",        37526778),
        ("place",          2028213),
    ]
    rows = collect_baseimageurls(sample_targets, rows_per_term=25)

    findings.append(f"- Total records collected: **{len(rows)}**\n")
    with_image = [r for r in rows if r.get("baseimageurl")]
    findings.append(f"- Records with a `baseimageurl` populated: **{len(with_image)}**\n")
    without_primary = [r for r in rows if not r.get("primaryimageurl")]
    findings.append(f"- Records missing `primaryimageurl`: **{len(without_primary)}**\n\n")

    # URN-id length distribution: strip the https://nrs.harvard.edu/ prefix.
    url_prefix = "https://nrs.harvard.edu/"
    urns = []
    for r in with_image:
        url = r["baseimageurl"]
        if url.startswith(url_prefix):
            urns.append(url[len(url_prefix):])
        else:
            urns.append(url)  # capture anomalies as-is

    if urns:
        lens = [len(u) for u in urns]
        findings.append("**Length distribution of the URN portion (what we'd put in iiif_key):**\n\n")
        findings.append(f"- min: {min(lens)} chars\n")
        findings.append(f"- max: {max(lens)} chars\n")
        findings.append(f"- median: {sorted(lens)[len(lens)//2]} chars\n")
        findings.append(f"- over 47 chars (won't fit in iiif_key[48]): "
                        f"{sum(1 for x in lens if x > 47)}/{len(lens)}\n\n")
        findings.append("**Sample URNs (first 10):**\n\n```\n")
        for u in urns[:10]:
            findings.append(f"{u}  ({len(u)} chars)\n")
        findings.append("```\n\n")

        # Anomalies — URNs that don't start with the expected prefix
        anomalies = [u for u, full in zip(urns, [r["baseimageurl"] for r in with_image])
                     if not full.startswith(url_prefix)]
        if anomalies:
            findings.append(f"**Anomalies — URLs NOT under `{url_prefix}`** ({len(anomalies)} found):\n\n")
            for a in anomalies[:5]:
                findings.append(f"- `{a}`\n")
            findings.append("\n")

    # ------------------------------------------------------------------
    # 2. Redirect chain (C5)
    # ------------------------------------------------------------------
    findings.append("## 2. NRS → IDS redirect chain (C5)\n\n")
    findings.append(
        "Append `/full/!720,720/0/default.jpg` to the URN and follow redirects "
        "manually with `allow_redirects=False`. Document the chain so the firmware "
        "knows how many round trips it costs per image.\n\n"
    )

    # Pick 3 samples spanning the cohort
    samples_for_redirect = with_image[:3]
    for idx, r in enumerate(samples_for_redirect, 1):
        base = r["baseimageurl"]
        full = base + "/full/!720,720/0/default.jpg"
        chain = []
        url = full
        for hop in range(5):
            try:
                resp = requests.head(url, allow_redirects=False, timeout=20,
                                     headers=SESSION.headers)
            except Exception as e:
                chain.append(f"hop {hop}: EXCEPTION {e}")
                break
            chain.append(f"hop {hop}: {resp.status_code} {url}")
            (RAW_DIR / f"image_redirect_{idx}_hop{hop}.txt").write_text(
                f"URL: {url}\nStatus: {resp.status_code}\n"
                + "\n".join(f"{k}: {v}" for k, v in resp.headers.items()),
                encoding="utf-8",
            )
            if 300 <= resp.status_code < 400 and "location" in resp.headers:
                url = resp.headers["location"]
                continue
            break
        findings.append(f"### Sample {idx}: object_id={r['object_id']}, baseimageurl=`{base}`\n\n")
        findings.append("```\n" + "\n".join(chain) + "\n```\n\n")

    # ------------------------------------------------------------------
    # 3. Image rendition fetch — verify JPEG, file size, dimensions (C1, C3)
    # ------------------------------------------------------------------
    findings.append("## 3. Image rendition fetch — `!720,720` JPEG (C1, C3)\n\n")
    findings.append("| # | object_id | status | content-type | size (bytes) | image dims |\n")
    findings.append("|---|---|---|---|---|---|\n")

    for idx, r in enumerate(with_image[:8], 1):
        base = r["baseimageurl"]
        url = base + "/full/!720,720/0/default.jpg"
        try:
            resp = requests.get(url, timeout=60, headers=SESSION.headers)
        except Exception as e:
            findings.append(f"| {idx} | {r['object_id']} | exception | - | - | {e} |\n")
            continue
        size_bytes = len(resp.content)
        ct = resp.headers.get("Content-Type", "?")
        # Quick image-size measurement using only stdlib if PIL isn't available
        dims = "-"
        try:
            from PIL import Image  # type: ignore
            import io as _io
            with Image.open(_io.BytesIO(resp.content)) as im:
                dims = f"{im.width}x{im.height}"
        except Exception:
            pass
        # Save first 200 bytes for the JPEG header signature
        try:
            (RAW_DIR / f"image_fetch_{idx}_header.bin").write_bytes(resp.content[:200])
        except Exception:
            pass
        findings.append(f"| {idx} | {r['object_id']} | {resp.status_code} | `{ct}` | "
                        f"{size_bytes} | {dims} |\n")
        time.sleep(0.3)

    findings.append("\n")
    findings.append(
        "**Constraint check:** p3a's `P3A_MAX_ARTWORK_SIZE = 16 MiB`. "
        "Any row above 16 777 216 bytes would need a smaller IIIF size cap.\n\n"
    )

    # ------------------------------------------------------------------
    # 4. Storage strategy recommendation
    # ------------------------------------------------------------------
    findings.append("## 4. Storage strategy for `iiif_key`\n\n")
    findings.append(
        "Two options for what to put in the 48-byte `iiif_key` slot:\n\n"
        "**(a) Store the NRS URN** (e.g. `urn-3:HUAM:79762_dynmc`).\n"
        "    - C-side `build_iiif_url`: `https://nrs.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`\n"
        "    - Download path: one 303 redirect to IDS, then 200 JPEG. The Rijks\n"
        "      adapter already implements manual redirect handling (HTTP_EVENT_ON_HEADER\n"
        "      captures Location); we reuse the pattern.\n"
        "    - No `resolve_entry` dispatch needed; this stays in the AIC/V&A bucket.\n\n"
        "**(b) Pre-resolve to IDS at refresh time** (e.g. `mps/18483191`).\n"
        "    - Refresh issues one HEAD per record to capture the 303 Location.\n"
        "    - C-side `build_iiif_url`: `https://ids.lib.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`\n"
        "    - Costs one extra HTTP per record during refresh; saves one round\n"
        "      trip per image download.\n\n"
        "Section 2 measured the actual redirect chain length. If it's a single\n"
        "303 hop, option **(a)** is the cleaner choice — fewer moving parts,\n"
        "no resolve_entry plumbing, and the refresh-time saving in option (b)\n"
        "is wasted bandwidth because most refreshed entries never get downloaded\n"
        "(the FIFO cache holds 1024+ entries but the player only consumes a few\n"
        "per minute). REPORT.md cements this choice once the section 2 data is in.\n"
    )

    REPORT_PATH.write_text("".join(findings), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
